//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/db_test_util.h"
#include "db/periodic_task_scheduler.h"
#include "db/seqno_to_time_mapping.h"
#include "port/stack_trace.h"
#include "rocksdb/iostats_context.h"
#include "rocksdb/utilities/debug.h"
#include "test_util/mock_time_env.h"

namespace ROCKSDB_NAMESPACE {

class SeqnoTimeTest : public DBTestBase {
 public:
  SeqnoTimeTest() : DBTestBase("seqno_time_test", /*env_do_fsync=*/false) {
    mock_clock_ = std::make_shared<MockSystemClock>(env_->GetSystemClock());
    mock_clock_->SetCurrentTime(kMockStartTime);
    mock_env_ = std::make_unique<CompositeEnvWrapper>(env_, mock_clock_);
  }

 protected:
  std::unique_ptr<Env> mock_env_;
  std::shared_ptr<MockSystemClock> mock_clock_;

  // Sufficient starting time that preserve time doesn't under-flow into
  // pre-history
  static constexpr uint32_t kMockStartTime = 10000000;

  void SetUp() override {
    mock_clock_->InstallTimedWaitFixCallback();
    SyncPoint::GetInstance()->SetCallBack(
        "DBImpl::StartPeriodicTaskScheduler:Init", [&](void* arg) {
          auto periodic_task_scheduler_ptr =
              reinterpret_cast<PeriodicTaskScheduler*>(arg);
          periodic_task_scheduler_ptr->TEST_OverrideTimer(mock_clock_.get());
        });
    mock_clock_->SetCurrentTime(kMockStartTime);
  }

  // make sure the file is not in cache, otherwise it won't have IO info
  void AssertKeyTemperature(int key_id, Temperature expected_temperature) {
    get_iostats_context()->Reset();
    IOStatsContext* iostats = get_iostats_context();
    std::string result = Get(Key(key_id));
    ASSERT_FALSE(result.empty());
    ASSERT_GT(iostats->bytes_read, 0);
    switch (expected_temperature) {
      case Temperature::kUnknown:
        ASSERT_EQ(iostats->file_io_stats_by_temperature.cold_file_read_count,
                  0);
        ASSERT_EQ(iostats->file_io_stats_by_temperature.cold_file_bytes_read,
                  0);
        break;
      case Temperature::kCold:
        ASSERT_GT(iostats->file_io_stats_by_temperature.cold_file_read_count,
                  0);
        ASSERT_GT(iostats->file_io_stats_by_temperature.cold_file_bytes_read,
                  0);
        break;
      default:
        // the test only support kCold now for the bottommost temperature
        FAIL();
    }
  }
};

TEST_F(SeqnoTimeTest, TemperatureBasicUniversal) {
  const int kNumTrigger = 4;
  const int kNumLevels = 7;
  const int kNumKeys = 100;
  const int kKeyPerSec = 10;

  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.preclude_last_level_data_seconds = 10000;
  options.env = mock_env_.get();
  options.bottommost_temperature = Temperature::kCold;
  options.num_levels = kNumLevels;
  DestroyAndReopen(options);

  // bootstrap DB sequence numbers (FIXME: make these steps unnecessary)
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_OK(SingleDelete("foo"));
  // pass some time first, otherwise the first a few keys write time are going
  // to be zero, and internally zero has special meaning: kUnknownTimeBeforeAll
  dbfull()->TEST_WaitForPeriodicTaskRun(
      [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(kKeyPerSec)); });

  int sst_num = 0;
  // Write files that are overlap and enough to trigger compaction
  for (; sst_num < kNumTrigger; sst_num++) {
    for (int i = 0; i < kNumKeys; i++) {
      ASSERT_OK(Put(Key(sst_num * (kNumKeys - 1) + i), "value"));
      dbfull()->TEST_WaitForPeriodicTaskRun([&] {
        mock_clock_->MockSleepForSeconds(static_cast<int>(kKeyPerSec));
      });
    }
    ASSERT_OK(Flush());
  }
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  // All data is hot, only output to penultimate level
  ASSERT_EQ("0,0,0,0,0,1", FilesPerLevel());
  ASSERT_GT(GetSstSizeHelper(Temperature::kUnknown), 0);
  ASSERT_EQ(GetSstSizeHelper(Temperature::kCold), 0);

  // read a random key, which should be hot (kUnknown)
  AssertKeyTemperature(20, Temperature::kUnknown);

  // Write more data, but still all hot until the 10th SST, as:
  // write a key every 10 seconds, 100 keys per SST, each SST takes 1000 seconds
  // The preclude_last_level_data_seconds is 10k
  for (; sst_num < kNumTrigger * 2; sst_num++) {
    for (int i = 0; i < kNumKeys; i++) {
      ASSERT_OK(Put(Key(sst_num * (kNumKeys - 1) + i), "value"));
      dbfull()->TEST_WaitForPeriodicTaskRun([&] {
        mock_clock_->MockSleepForSeconds(static_cast<int>(kKeyPerSec));
      });
    }
    ASSERT_OK(Flush());
    ASSERT_OK(dbfull()->TEST_WaitForCompact());
    ASSERT_GT(GetSstSizeHelper(Temperature::kUnknown), 0);
    ASSERT_EQ(GetSstSizeHelper(Temperature::kCold), 0);
  }

  // Now we have both hot data and cold data
  for (; sst_num < kNumTrigger * 3; sst_num++) {
    for (int i = 0; i < kNumKeys; i++) {
      ASSERT_OK(Put(Key(sst_num * (kNumKeys - 1) + i), "value"));
      dbfull()->TEST_WaitForPeriodicTaskRun([&] {
        mock_clock_->MockSleepForSeconds(static_cast<int>(kKeyPerSec));
      });
    }
    ASSERT_OK(Flush());
    ASSERT_OK(dbfull()->TEST_WaitForCompact());
  }

  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));
  uint64_t hot_data_size = GetSstSizeHelper(Temperature::kUnknown);
  uint64_t cold_data_size = GetSstSizeHelper(Temperature::kCold);
  ASSERT_GT(hot_data_size, 0);
  ASSERT_GT(cold_data_size, 0);
  // the first a few key should be cold
  AssertKeyTemperature(20, Temperature::kCold);

  for (int i = 0; i < 30; i++) {
    dbfull()->TEST_WaitForPeriodicTaskRun([&] {
      mock_clock_->MockSleepForSeconds(static_cast<int>(20 * kKeyPerSec));
    });
    ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));

    // the hot/cold data cut off range should be between i * 20 + 200 -> 250
    AssertKeyTemperature(i * 20 + 250, Temperature::kUnknown);
    AssertKeyTemperature(i * 20 + 200, Temperature::kCold);
  }

  ASSERT_LT(GetSstSizeHelper(Temperature::kUnknown), hot_data_size);
  ASSERT_GT(GetSstSizeHelper(Temperature::kCold), cold_data_size);

  // Wait again, the most of the data should be cold after that
  // but it may not be all cold, because if there's no new data write to SST,
  // the compaction will not get the new seqno->time sampling to decide the last
  // a few data's time.
  for (int i = 0; i < 5; i++) {
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(1000)); });
    ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));
  }

  // any random data close to the end should be cold
  AssertKeyTemperature(1000, Temperature::kCold);

  // close explicitly, because the env is local variable which will be released
  // first.
  Close();
}

TEST_F(SeqnoTimeTest, TemperatureBasicLevel) {
  const int kNumLevels = 7;
  const int kNumKeys = 100;

  Options options = CurrentOptions();
  options.preclude_last_level_data_seconds = 10000;
  options.env = mock_env_.get();
  options.bottommost_temperature = Temperature::kCold;
  options.num_levels = kNumLevels;
  options.level_compaction_dynamic_level_bytes = true;
  // TODO(zjay): for level compaction, auto-compaction may stuck in deadloop, if
  //  the penultimate level score > 1, but the hot is not cold enough to compact
  //  to last level, which will keep triggering compaction.
  options.disable_auto_compactions = true;
  DestroyAndReopen(options);

  // bootstrap DB sequence numbers (FIXME: make these steps unnecessary)
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_OK(SingleDelete("foo"));
  // pass some time first, otherwise the first a few keys write time are going
  // to be zero, and internally zero has special meaning: kUnknownTimeBeforeAll
  dbfull()->TEST_WaitForPeriodicTaskRun(
      [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(10)); });

  int sst_num = 0;
  // Write files that are overlap
  for (; sst_num < 4; sst_num++) {
    for (int i = 0; i < kNumKeys; i++) {
      ASSERT_OK(Put(Key(sst_num * (kNumKeys - 1) + i), "value"));
      dbfull()->TEST_WaitForPeriodicTaskRun(
          [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(10)); });
    }
    ASSERT_OK(Flush());
  }

  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));

  // All data is hot, only output to penultimate level
  ASSERT_EQ("0,0,0,0,0,1", FilesPerLevel());
  ASSERT_GT(GetSstSizeHelper(Temperature::kUnknown), 0);
  ASSERT_EQ(GetSstSizeHelper(Temperature::kCold), 0);

  // read a random key, which should be hot (kUnknown)
  AssertKeyTemperature(20, Temperature::kUnknown);

  // Adding more data to have mixed hot and cold data
  for (; sst_num < 14; sst_num++) {
    for (int i = 0; i < kNumKeys; i++) {
      ASSERT_OK(Put(Key(sst_num * (kNumKeys - 1) + i), "value"));
      dbfull()->TEST_WaitForPeriodicTaskRun(
          [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(10)); });
    }
    ASSERT_OK(Flush());
  }
  // Second to last level
  MoveFilesToLevel(5);
  ASSERT_GT(GetSstSizeHelper(Temperature::kUnknown), 0);
  ASSERT_EQ(GetSstSizeHelper(Temperature::kCold), 0);

  // Compact the files to the last level which should split the hot/cold data
  MoveFilesToLevel(6);
  uint64_t hot_data_size = GetSstSizeHelper(Temperature::kUnknown);
  uint64_t cold_data_size = GetSstSizeHelper(Temperature::kCold);
  ASSERT_GT(hot_data_size, 0);
  ASSERT_GT(cold_data_size, 0);
  // the first a few key should be cold
  AssertKeyTemperature(20, Temperature::kCold);

  // Wait some time, with each wait, the cold data is increasing and hot data is
  // decreasing
  for (int i = 0; i < 30; i++) {
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(200)); });
    ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));
    uint64_t pre_hot = hot_data_size;
    uint64_t pre_cold = cold_data_size;
    hot_data_size = GetSstSizeHelper(Temperature::kUnknown);
    cold_data_size = GetSstSizeHelper(Temperature::kCold);
    ASSERT_LT(hot_data_size, pre_hot);
    ASSERT_GT(cold_data_size, pre_cold);

    // the hot/cold cut_off key should be around i * 20 + 400 -> 450
    AssertKeyTemperature(i * 20 + 450, Temperature::kUnknown);
    AssertKeyTemperature(i * 20 + 400, Temperature::kCold);
  }

  // Wait again, the most of the data should be cold after that
  // hot data might not be empty, because if we don't write new data, there's
  // no seqno->time sampling available to the compaction
  for (int i = 0; i < 5; i++) {
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(1000)); });
    ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));
  }

  // any random data close to the end should be cold
  AssertKeyTemperature(1000, Temperature::kCold);

  Close();
}

enum class SeqnoTimeTestType : char {
  kTrackInternalTimeSeconds = 0,
  kPrecludeLastLevel = 1,
  kBothSetTrackSmaller = 2,
};

class SeqnoTimeTablePropTest
    : public SeqnoTimeTest,
      public ::testing::WithParamInterface<SeqnoTimeTestType> {
 public:
  SeqnoTimeTablePropTest() : SeqnoTimeTest() {}

  void SetTrackTimeDurationOptions(uint64_t track_time_duration,
                                   Options& options) const {
    // either option set will enable the time tracking feature
    switch (GetParam()) {
      case SeqnoTimeTestType::kTrackInternalTimeSeconds:
        options.preclude_last_level_data_seconds = 0;
        options.preserve_internal_time_seconds = track_time_duration;
        break;
      case SeqnoTimeTestType::kPrecludeLastLevel:
        options.preclude_last_level_data_seconds = track_time_duration;
        options.preserve_internal_time_seconds = 0;
        break;
      case SeqnoTimeTestType::kBothSetTrackSmaller:
        options.preclude_last_level_data_seconds = track_time_duration;
        options.preserve_internal_time_seconds = track_time_duration / 10;
        break;
    }
  }
};

INSTANTIATE_TEST_CASE_P(
    SeqnoTimeTablePropTest, SeqnoTimeTablePropTest,
    ::testing::Values(SeqnoTimeTestType::kTrackInternalTimeSeconds,
                      SeqnoTimeTestType::kPrecludeLastLevel,
                      SeqnoTimeTestType::kBothSetTrackSmaller));

TEST_P(SeqnoTimeTablePropTest, BasicSeqnoToTimeMapping) {
  Options options = CurrentOptions();
  SetTrackTimeDurationOptions(10000, options);

  options.env = mock_env_.get();
  options.disable_auto_compactions = true;
  DestroyAndReopen(options);

  // bootstrap DB sequence numbers (FIXME: make these steps unnecessary)
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_OK(SingleDelete("foo"));
  // pass some time first, otherwise the first a few keys write time are going
  // to be zero, and internally zero has special meaning: kUnknownTimeBeforeAll
  dbfull()->TEST_WaitForPeriodicTaskRun(
      [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });

  std::set<uint64_t> checked_file_nums;
  SequenceNumber start_seq = dbfull()->GetLatestSequenceNumber() + 1;
  uint64_t start_time = mock_clock_->NowSeconds();

  // Write a key every 10 seconds
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(Key(i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(10)); });
  }
  ASSERT_OK(Flush());
  TablePropertiesCollection tables_props;
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));
  ASSERT_EQ(tables_props.size(), 1);
  auto it = tables_props.begin();
  SeqnoToTimeMapping tp_mapping;
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));
  ASSERT_OK(tp_mapping.Sort());
  ASSERT_FALSE(tp_mapping.Empty());
  auto seqs = tp_mapping.TEST_GetInternalMapping();
  // about ~20 seqs->time entries, because the sample rate is 10000/100, and it
  // passes 2k time.
  ASSERT_GE(seqs.size(), 19);
  ASSERT_LE(seqs.size(), 21);
  SequenceNumber seq_end = dbfull()->GetLatestSequenceNumber() + 1;
  for (auto i = start_seq; i < seq_end; i++) {
    // The result is within the range
    ASSERT_GE(tp_mapping.GetProximalTimeBeforeSeqno(i),
              start_time + (i - start_seq) * 10 - 100);
    ASSERT_LE(tp_mapping.GetProximalTimeBeforeSeqno(i),
              start_time + (i - start_seq) * 10);
  }
  checked_file_nums.insert(it->second->orig_file_number);
  start_seq = seq_end;
  start_time = mock_clock_->NowSeconds();

  // Write a key every 1 seconds
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(Key(i + 190), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(1)); });
  }
  seq_end = dbfull()->GetLatestSequenceNumber() + 1;
  ASSERT_OK(Flush());
  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));
  ASSERT_EQ(tables_props.size(), 2);
  it = tables_props.begin();
  while (it != tables_props.end()) {
    if (!checked_file_nums.count(it->second->orig_file_number)) {
      break;
    }
    it++;
  }
  ASSERT_TRUE(it != tables_props.end());

  tp_mapping.Clear();
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));
  ASSERT_OK(tp_mapping.Sort());
  seqs = tp_mapping.TEST_GetInternalMapping();
  // There only a few time sample
  ASSERT_GE(seqs.size(), 1);
  ASSERT_LE(seqs.size(), 3);
  for (auto i = start_seq; i < seq_end; i++) {
    ASSERT_GE(tp_mapping.GetProximalTimeBeforeSeqno(i),
              start_time + (i - start_seq) - 100);
    ASSERT_LE(tp_mapping.GetProximalTimeBeforeSeqno(i),
              start_time + (i - start_seq));
  }
  checked_file_nums.insert(it->second->orig_file_number);
  start_seq = seq_end;
  start_time = mock_clock_->NowSeconds();

  // Write a key every 200 seconds
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(Key(i + 380), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(200)); });
  }
  seq_end = dbfull()->GetLatestSequenceNumber() + 1;
  ASSERT_OK(Flush());
  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));
  ASSERT_EQ(tables_props.size(), 3);
  it = tables_props.begin();
  while (it != tables_props.end()) {
    if (!checked_file_nums.count(it->second->orig_file_number)) {
      break;
    }
    it++;
  }
  ASSERT_TRUE(it != tables_props.end());

  tp_mapping.Clear();
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));
  ASSERT_OK(tp_mapping.Sort());
  seqs = tp_mapping.TEST_GetInternalMapping();
  // The sequence number -> time entries should be maxed
  ASSERT_GE(seqs.size(), 99);
  ASSERT_LE(seqs.size(), 101);
  for (auto i = start_seq; i < seq_end; i++) {
    // aged out entries allowed to report time=0
    if ((seq_end - i) * 200 <= 10000) {
      ASSERT_GE(tp_mapping.GetProximalTimeBeforeSeqno(i),
                start_time + (i - start_seq) * 200 - 100);
    }
    ASSERT_LE(tp_mapping.GetProximalTimeBeforeSeqno(i),
              start_time + (i - start_seq) * 200);
  }
  checked_file_nums.insert(it->second->orig_file_number);
  start_seq = seq_end;
  start_time = mock_clock_->NowSeconds();

  // Write a key every 100 seconds
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(Key(i + 570), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
  }
  seq_end = dbfull()->GetLatestSequenceNumber() + 1;
  ASSERT_OK(Flush());
  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));
  ASSERT_EQ(tables_props.size(), 4);
  it = tables_props.begin();
  while (it != tables_props.end()) {
    if (!checked_file_nums.count(it->second->orig_file_number)) {
      break;
    }
    it++;
  }
  ASSERT_TRUE(it != tables_props.end());
  tp_mapping.Clear();
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));
  ASSERT_OK(tp_mapping.Sort());
  seqs = tp_mapping.TEST_GetInternalMapping();
  ASSERT_GE(seqs.size(), 99);
  ASSERT_LE(seqs.size(), 101);

  checked_file_nums.insert(it->second->orig_file_number);

  // re-enable compaction
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "false"},
  }));

  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));
  ASSERT_GE(tables_props.size(), 1);
  it = tables_props.begin();
  while (it != tables_props.end()) {
    if (!checked_file_nums.count(it->second->orig_file_number)) {
      break;
    }
    it++;
  }
  ASSERT_TRUE(it != tables_props.end());
  tp_mapping.Clear();
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));
  ASSERT_OK(tp_mapping.Sort());
  seqs = tp_mapping.TEST_GetInternalMapping();
  ASSERT_GE(seqs.size(), 99);
  ASSERT_LE(seqs.size(), 101);
  for (auto i = start_seq; i < seq_end; i++) {
    // aged out entries allowed to report time=0
    // FIXME: should be <=
    if ((seq_end - i) * 100 < 10000) {
      ASSERT_GE(tp_mapping.GetProximalTimeBeforeSeqno(i),
                start_time + (i - start_seq) * 100 - 100);
    }
    ASSERT_LE(tp_mapping.GetProximalTimeBeforeSeqno(i),
              start_time + (i - start_seq) * 100);
  }
  ASSERT_OK(db_->Close());
}

TEST_P(SeqnoTimeTablePropTest, MultiCFs) {
  Options options = CurrentOptions();
  options.preclude_last_level_data_seconds = 0;
  options.preserve_internal_time_seconds = 0;
  options.env = mock_env_.get();
  options.stats_dump_period_sec = 0;
  options.stats_persist_period_sec = 0;
  ReopenWithColumnFamilies({"default"}, options);

  const PeriodicTaskScheduler& scheduler =
      dbfull()->TEST_GetPeriodicTaskScheduler();
  ASSERT_FALSE(scheduler.TEST_HasTask(PeriodicTaskType::kRecordSeqnoTime));

  // Write some data and increase the current time
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(Key(i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
  }
  ASSERT_OK(Flush());
  TablePropertiesCollection tables_props;
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));
  ASSERT_EQ(tables_props.size(), 1);
  auto it = tables_props.begin();
  ASSERT_TRUE(it->second->seqno_to_time_mapping.empty());

  ASSERT_TRUE(dbfull()->TEST_GetSeqnoToTimeMapping().Empty());

  Options options_1 = options;
  SetTrackTimeDurationOptions(10000, options_1);
  CreateColumnFamilies({"one"}, options_1);
  ASSERT_TRUE(scheduler.TEST_HasTask(PeriodicTaskType::kRecordSeqnoTime));

  // Write some data to the default CF (without preclude_last_level feature)
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(Key(i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
  }
  ASSERT_OK(Flush());

  // Write some data to the CF one
  for (int i = 0; i < 20; i++) {
    ASSERT_OK(Put(1, Key(i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(10)); });
  }
  ASSERT_OK(Flush(1));
  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(handles_[1], &tables_props));
  ASSERT_EQ(tables_props.size(), 1);
  it = tables_props.begin();
  SeqnoToTimeMapping tp_mapping;
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));
  ASSERT_OK(tp_mapping.Sort());
  ASSERT_FALSE(tp_mapping.Empty());
  auto seqs = tp_mapping.TEST_GetInternalMapping();
  ASSERT_GE(seqs.size(), 1);
  ASSERT_LE(seqs.size(), 4);

  // Create one more CF with larger preclude_last_level time
  Options options_2 = options;
  SetTrackTimeDurationOptions(1000000, options_2);  // 1m
  CreateColumnFamilies({"two"}, options_2);

  // Add more data to CF "two" to fill the in memory mapping
  for (int i = 0; i < 2000; i++) {
    ASSERT_OK(Put(2, Key(i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
  }
  seqs = dbfull()->TEST_GetSeqnoToTimeMapping().TEST_GetInternalMapping();
  ASSERT_GE(seqs.size(), 1000 - 1);
  ASSERT_LE(seqs.size(), 1000 + 1);

  ASSERT_OK(Flush(2));
  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(handles_[2], &tables_props));
  ASSERT_EQ(tables_props.size(), 1);
  it = tables_props.begin();
  tp_mapping.Clear();
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));
  ASSERT_OK(tp_mapping.Sort());
  seqs = tp_mapping.TEST_GetInternalMapping();
  // the max encoded entries is 100
  ASSERT_GE(seqs.size(), 100 - 1);
  ASSERT_LE(seqs.size(), 100 + 1);

  // Write some data to default CF, as all memtable with preclude_last_level
  // enabled have flushed, the in-memory seqno->time mapping should be cleared
  for (int i = 0; i < 10; i++) {
    ASSERT_OK(Put(0, Key(i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
  }
  seqs = dbfull()->TEST_GetSeqnoToTimeMapping().TEST_GetInternalMapping();
  ASSERT_OK(Flush(0));

  // trigger compaction for CF "two" and make sure the compaction output has
  // seqno_to_time_mapping
  for (int j = 0; j < 3; j++) {
    for (int i = 0; i < 200; i++) {
      ASSERT_OK(Put(2, Key(i), "value"));
      dbfull()->TEST_WaitForPeriodicTaskRun(
          [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
    }
    ASSERT_OK(Flush(2));
  }
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(handles_[2], &tables_props));
  ASSERT_EQ(tables_props.size(), 1);
  it = tables_props.begin();
  tp_mapping.Clear();
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));
  ASSERT_OK(tp_mapping.Sort());
  seqs = tp_mapping.TEST_GetInternalMapping();
  ASSERT_GE(seqs.size(), 99);
  ASSERT_LE(seqs.size(), 101);

    for (int i = 0; i < 200; i++) {
      ASSERT_OK(Put(0, Key(i), "value"));
      dbfull()->TEST_WaitForPeriodicTaskRun(
          [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
    }
    ASSERT_OK(Flush(0));
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(handles_[0], &tables_props));
  ASSERT_EQ(tables_props.size(), 1);
  it = tables_props.begin();
  ASSERT_TRUE(it->second->seqno_to_time_mapping.empty());

  // Write some data to CF "two", but don't flush to accumulate
  for (int i = 0; i < 1000; i++) {
    ASSERT_OK(Put(2, Key(i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
  }
  ASSERT_GE(
      dbfull()->TEST_GetSeqnoToTimeMapping().TEST_GetInternalMapping().size(),
      500);
  // After dropping CF "one", the in-memory mapping will be change to only
  // follow CF "two" options.
  ASSERT_OK(db_->DropColumnFamily(handles_[1]));
  ASSERT_LE(
      dbfull()->TEST_GetSeqnoToTimeMapping().TEST_GetInternalMapping().size(),
      100 + 5);

  // After dropping CF "two", the in-memory mapping is also clear.
  ASSERT_OK(db_->DropColumnFamily(handles_[2]));
  ASSERT_EQ(
      dbfull()->TEST_GetSeqnoToTimeMapping().TEST_GetInternalMapping().size(),
      0);

  // And the timer worker is stopped
  ASSERT_FALSE(scheduler.TEST_HasTask(PeriodicTaskType::kRecordSeqnoTime));
  Close();
}

TEST_P(SeqnoTimeTablePropTest, MultiInstancesBasic) {
  const int kInstanceNum = 2;

  Options options = CurrentOptions();
  SetTrackTimeDurationOptions(10000, options);
  options.env = mock_env_.get();
  options.stats_dump_period_sec = 0;
  options.stats_persist_period_sec = 0;

  auto dbs = std::vector<DB*>(kInstanceNum);
  for (int i = 0; i < kInstanceNum; i++) {
    ASSERT_OK(
        DB::Open(options, test::PerThreadDBPath(std::to_string(i)), &(dbs[i])));
  }

  // Make sure the second instance has the worker enabled
  auto dbi = static_cast_with_check<DBImpl>(dbs[1]);
  WriteOptions wo;
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(dbi->Put(wo, Key(i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(100)); });
  }
  SeqnoToTimeMapping seqno_to_time_mapping = dbi->TEST_GetSeqnoToTimeMapping();
  ASSERT_GT(seqno_to_time_mapping.Size(), 10);

  for (int i = 0; i < kInstanceNum; i++) {
    ASSERT_OK(dbs[i]->Close());
    delete dbs[i];
  }
}

TEST_P(SeqnoTimeTablePropTest, SeqnoToTimeMappingUniversal) {
  const int kNumTrigger = 4;
  const int kNumLevels = 7;
  const int kNumKeys = 100;

  Options options = CurrentOptions();
  SetTrackTimeDurationOptions(10000, options);
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = kNumLevels;
  options.env = mock_env_.get();

  DestroyAndReopen(options);

  // bootstrap DB sequence numbers (FIXME: make these steps unnecessary)
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_OK(SingleDelete("foo"));
  // pass some time first, otherwise the first a few keys write time are going
  // to be zero, and internally zero has special meaning: kUnknownTimeBeforeAll
  dbfull()->TEST_WaitForPeriodicTaskRun(
      [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(10)); });

  std::atomic_uint64_t num_seqno_zeroing{0};

  SyncPoint::GetInstance()->DisableProcessing();
  SyncPoint::GetInstance()->ClearAllCallBacks();
  SyncPoint::GetInstance()->SetCallBack(
      "CompactionIterator::PrepareOutput:ZeroingSeq",
      [&](void* /*arg*/) { num_seqno_zeroing++; });
  SyncPoint::GetInstance()->EnableProcessing();

  int sst_num = 0;
  for (; sst_num < kNumTrigger - 1; sst_num++) {
    for (int i = 0; i < kNumKeys; i++) {
      ASSERT_OK(Put(Key(sst_num * (kNumKeys - 1) + i), "value"));
      dbfull()->TEST_WaitForPeriodicTaskRun(
          [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(10)); });
    }
    ASSERT_OK(Flush());
  }
  TablePropertiesCollection tables_props;
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));
  ASSERT_EQ(tables_props.size(), 3);
  for (const auto& props : tables_props) {
    ASSERT_FALSE(props.second->seqno_to_time_mapping.empty());
    SeqnoToTimeMapping tp_mapping;
    ASSERT_OK(tp_mapping.Add(props.second->seqno_to_time_mapping));
    ASSERT_OK(tp_mapping.Sort());
    ASSERT_FALSE(tp_mapping.Empty());
    auto seqs = tp_mapping.TEST_GetInternalMapping();
    ASSERT_GE(seqs.size(), 10 - 1);
    ASSERT_LE(seqs.size(), 10 + 1);
  }

  // Trigger a compaction
  for (int i = 0; i < kNumKeys; i++) {
    ASSERT_OK(Put(Key(sst_num * (kNumKeys - 1) + i), "value"));
    dbfull()->TEST_WaitForPeriodicTaskRun(
        [&] { mock_clock_->MockSleepForSeconds(static_cast<int>(10)); });
  }
  sst_num++;
  ASSERT_OK(Flush());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  tables_props.clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));
  ASSERT_EQ(tables_props.size(), 1);

  auto it = tables_props.begin();
  SeqnoToTimeMapping tp_mapping;
  ASSERT_FALSE(it->second->seqno_to_time_mapping.empty());
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));

  // compact to the last level
  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));
  // make sure the data is all compacted to penultimate level if the feature is
  // on, otherwise, compacted to the last level.
  if (options.preclude_last_level_data_seconds > 0) {
    ASSERT_GT(NumTableFilesAtLevel(5), 0);
    ASSERT_EQ(NumTableFilesAtLevel(6), 0);
  } else {
    ASSERT_EQ(NumTableFilesAtLevel(5), 0);
    ASSERT_GT(NumTableFilesAtLevel(6), 0);
  }

  // regardless the file is on the last level or not, it should keep the time
  // information and sequence number are not set
  tables_props.clear();
  tp_mapping.Clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));

  ASSERT_EQ(tables_props.size(), 1);
  ASSERT_EQ(num_seqno_zeroing, 0);

  it = tables_props.begin();
  ASSERT_FALSE(it->second->seqno_to_time_mapping.empty());
  ASSERT_OK(tp_mapping.Add(it->second->seqno_to_time_mapping));

  // make half of the data expired
  mock_clock_->MockSleepForSeconds(static_cast<int>(8000));
  ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));

  tables_props.clear();
  tp_mapping.Clear();
  ASSERT_OK(dbfull()->GetPropertiesOfAllTables(&tables_props));

  if (options.preclude_last_level_data_seconds > 0) {
    ASSERT_EQ(tables_props.size(), 2);
  } else {
    ASSERT_EQ(tables_props.size(), 1);
  }
  ASSERT_GT(num_seqno_zeroing, 0);
  std::vector<KeyVersion> key_versions;
  ASSERT_OK(GetAllKeyVersions(db_, Slice(), Slice(),
                              std::numeric_limits<size_t>::max(),
                              &key_versions));
  // make sure there're more than 300 keys and first 100 keys are having seqno
  // zeroed out, the last 100 key seqno not zeroed out
  ASSERT_GT(key_versions.size(), 300);
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(key_versions[i].sequence, 0);
  }
  auto rit = key_versions.rbegin();
  for (int i = 0; i < 100; i++) {
    ASSERT_GT(rit->sequence, 0);
    rit++;
  }

  // make all data expired and compact again to push it to the last level
  // regardless if the tiering feature is enabled or not
  mock_clock_->MockSleepForSeconds(static_cast<int>(20000));

  ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));

  ASSERT_GT(num_seqno_zeroing, 0);
  ASSERT_GT(NumTableFilesAtLevel(6), 0);

  Close();
}

TEST_F(SeqnoTimeTest, MappingAppend) {
  SeqnoToTimeMapping test(/*max_time_duration=*/100, /*max_capacity=*/10);

  // ignore seqno == 0, as it may mean the seqno is zeroed out
  ASSERT_FALSE(test.Append(0, 9));

  ASSERT_TRUE(test.Append(3, 10));
  auto size = test.Size();
  // normal add
  ASSERT_TRUE(test.Append(10, 11));
  size++;
  ASSERT_EQ(size, test.Size());

  // Append unsorted
  ASSERT_FALSE(test.Append(8, 12));
  ASSERT_EQ(size, test.Size());

  // Append with the same seqno, newer time is rejected because that makes
  // GetProximalSeqnoBeforeTime queries worse (see later test)
  ASSERT_FALSE(test.Append(10, 12));
  ASSERT_EQ(size, test.Size());
  // older time will be ignored
  ASSERT_FALSE(test.Append(10, 9));
  ASSERT_EQ(size, test.Size());

  // new seqno with old time will be ignored
  ASSERT_FALSE(test.Append(12, 8));
  ASSERT_EQ(size, test.Size());

  // new seqno with same time is accepted by replacing last entry
  // (improves GetProximalSeqnoBeforeTime queries without blowing up size)
  ASSERT_TRUE(test.Append(12, 11));
  ASSERT_EQ(size, test.Size());
}

TEST_F(SeqnoTimeTest, ProximalFunctions) {
  SeqnoToTimeMapping test(/*max_time_duration=*/100, /*max_capacity=*/10);

  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(1), kUnknownTimeBeforeAll);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(1000000000000U),
            kUnknownTimeBeforeAll);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(1), kUnknownSeqnoBeforeAll);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(1000000000000U),
            kUnknownSeqnoBeforeAll);

  // (Taken from example in SeqnoToTimeMapping class comment)
  // Time 500 is after seqno 10 and before seqno 11
  EXPECT_TRUE(test.Append(10, 500));

  // Seqno too early
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(9), kUnknownTimeBeforeAll);
  // We only know that 500 is after 10
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(10), kUnknownTimeBeforeAll);
  // Found
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(11), 500U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(1000000000000U), 500U);

  // Time too early
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(499), kUnknownSeqnoBeforeAll);
  // Found
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(500), 10U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(501), 10U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(1000000000000U), 10U);

  // More samples
  EXPECT_TRUE(test.Append(20, 600));
  EXPECT_TRUE(test.Append(30, 700));

  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(10), kUnknownTimeBeforeAll);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(11), 500U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(20), 500U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(21), 600U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(30), 600U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(31), 700U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(1000000000000U), 700U);

  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(499), kUnknownSeqnoBeforeAll);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(500), 10U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(501), 10U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(599), 10U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(600), 20U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(601), 20U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(699), 20U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(700), 30U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(701), 30U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(1000000000000U), 30U);

  // Redundant sample ignored
  EXPECT_EQ(test.Size(), 3U);
  EXPECT_FALSE(test.Append(30, 700));
  EXPECT_EQ(test.Size(), 3U);

  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(30), 600U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(31), 700U);

  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(699), 20U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(700), 30U);

  // Later sample with same seqno is ignored, to provide best results
  // for GetProximalSeqnoBeforeTime function while saving entries
  // in SeqnoToTimeMapping.
  EXPECT_FALSE(test.Append(30, 800));

  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(30), 600U);
  // Could return 800, but saving space in SeqnoToTimeMapping instead.
  // Can reconsider if/when GetProximalTimeBeforeSeqno is used in
  // production.
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(31), 700U);

  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(699), 20U);
  // If the existing {30, 700} entry were replaced with {30, 800}, this
  // would return seqno 20 instead of 30, which would preclude more than
  // necessary for "preclude_last_level_data_seconds" feature.
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(700), 30U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(800), 30U);

  // Still OK
  EXPECT_TRUE(test.Append(40, 900));

  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(30), 600U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(41), 900U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(899), 30U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(900), 40U);

  // Burst of writes during a short time creates an opportunity
  // for better results from GetProximalSeqnoBeforeTime(), at the
  // expense of GetProximalTimeBeforeSeqno().
  EXPECT_TRUE(test.Append(50, 900));

  // These are subject to later revision depending on priorities
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(49), 700U);
  EXPECT_EQ(test.GetProximalTimeBeforeSeqno(51), 900U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(899), 30U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(900), 50U);
}

TEST_F(SeqnoTimeTest, TruncateOldEntries) {
  constexpr uint64_t kMaxTimeDuration = 42;
  SeqnoToTimeMapping test(kMaxTimeDuration, /*max_capacity=*/10);

  EXPECT_EQ(test.Size(), 0U);

  // Safe on empty mapping
  test.TruncateOldEntries(500);

  EXPECT_EQ(test.Size(), 0U);

  // (Taken from example in SeqnoToTimeMapping class comment)
  // Time 500 is after seqno 10 and before seqno 11
  EXPECT_TRUE(test.Append(10, 500));
  EXPECT_TRUE(test.Append(20, 600));
  EXPECT_TRUE(test.Append(30, 700));
  EXPECT_TRUE(test.Append(40, 800));
  EXPECT_TRUE(test.Append(50, 900));

  EXPECT_EQ(test.Size(), 5U);

  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(500), 10U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(599), 10U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(600), 20U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(699), 20U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(700), 30U);
  // etc.

  // Must keep first entry
  test.TruncateOldEntries(500 + kMaxTimeDuration);
  EXPECT_EQ(test.Size(), 5U);
  test.TruncateOldEntries(599 + kMaxTimeDuration);
  EXPECT_EQ(test.Size(), 5U);

  // Purges first entry
  test.TruncateOldEntries(600 + kMaxTimeDuration);
  EXPECT_EQ(test.Size(), 4U);

  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(500), kUnknownSeqnoBeforeAll);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(599), kUnknownSeqnoBeforeAll);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(600), 20U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(699), 20U);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(700), 30U);

  // No effect
  test.TruncateOldEntries(600 + kMaxTimeDuration);
  EXPECT_EQ(test.Size(), 4U);
  test.TruncateOldEntries(699 + kMaxTimeDuration);
  EXPECT_EQ(test.Size(), 4U);

  // Purges next two
  test.TruncateOldEntries(899 + kMaxTimeDuration);
  EXPECT_EQ(test.Size(), 2U);

  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(799), kUnknownSeqnoBeforeAll);
  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(899), 40U);

  // Always keep last entry, to have a non-trivial seqno bound
  test.TruncateOldEntries(10000000);
  EXPECT_EQ(test.Size(), 1U);

  EXPECT_EQ(test.GetProximalSeqnoBeforeTime(10000000), 50U);
}

TEST_F(SeqnoTimeTest, Sort) {
  SeqnoToTimeMapping test;

  // single entry
  test.Add(10, 11);
  ASSERT_OK(test.Sort());
  ASSERT_EQ(test.Size(), 1);

  // duplicate, should be removed by sort
  test.Add(10, 11);
  // same seqno, but older time, should be removed
  test.Add(10, 9);

  // unuseful ones, should be removed by sort
  test.Add(11, 9);
  test.Add(9, 8);

  // Good ones
  test.Add(1, 10);
  test.Add(100, 100);

  ASSERT_OK(test.Sort());

  auto seqs = test.TEST_GetInternalMapping();

  std::deque<SeqnoToTimeMapping::SeqnoTimePair> expected;
  expected.emplace_back(1, 10);
  expected.emplace_back(10, 11);
  expected.emplace_back(100, 100);

  ASSERT_EQ(expected, seqs);
}

TEST_F(SeqnoTimeTest, EncodeDecodeBasic) {
  SeqnoToTimeMapping test(0, 1000);

  std::string output;
  test.Encode(output, 0, 1000, 100);
  ASSERT_TRUE(output.empty());

  for (int i = 1; i <= 1000; i++) {
    ASSERT_TRUE(test.Append(i, i * 10));
  }
  test.Encode(output, 0, 1000, 100);

  ASSERT_FALSE(output.empty());

  SeqnoToTimeMapping decoded;
  ASSERT_OK(decoded.Add(output));
  ASSERT_OK(decoded.Sort());
  ASSERT_EQ(decoded.Size(), SeqnoToTimeMapping::kMaxSeqnoTimePairsPerSST);
  ASSERT_EQ(test.Size(), 1000);

  for (SequenceNumber seq = 0; seq <= 1000; seq++) {
    // test has the more accurate time mapping, encode only pick
    // kMaxSeqnoTimePairsPerSST number of entries, which is less accurate
    uint64_t target_time = test.GetProximalTimeBeforeSeqno(seq);
    ASSERT_GE(decoded.GetProximalTimeBeforeSeqno(seq),
              target_time < 200 ? 0 : target_time - 200);
    ASSERT_LE(decoded.GetProximalTimeBeforeSeqno(seq), target_time);
  }
}

TEST_F(SeqnoTimeTest, EncodeDecodePerferNewTime) {
  SeqnoToTimeMapping test(0, 10);

  test.Append(1, 10);
  test.Append(5, 17);
  test.Append(6, 25);
  test.Append(8, 30);

  std::string output;
  test.Encode(output, 1, 10, 0, 3);

  SeqnoToTimeMapping decoded;
  ASSERT_OK(decoded.Add(output));
  ASSERT_OK(decoded.Sort());

  ASSERT_EQ(decoded.Size(), 3);

  auto seqs = decoded.TEST_GetInternalMapping();
  std::deque<SeqnoToTimeMapping::SeqnoTimePair> expected;
  expected.emplace_back(1, 10);
  expected.emplace_back(6, 25);
  expected.emplace_back(8, 30);
  ASSERT_EQ(expected, seqs);

  // Add a few large time number
  test.Append(10, 100);
  test.Append(13, 200);
  test.Append(16, 300);

  output.clear();
  test.Encode(output, 1, 20, 0, 4);
  decoded.Clear();
  ASSERT_OK(decoded.Add(output));
  ASSERT_OK(decoded.Sort());
  ASSERT_EQ(decoded.Size(), 4);

  expected.clear();
  expected.emplace_back(1, 10);
  // entry #6, #8 are skipped as they are too close to #1.
  // entry #100 is also within skip range, but if it's skipped, there not enough
  // number to fill 4 entries, so select it.
  expected.emplace_back(10, 100);
  expected.emplace_back(13, 200);
  expected.emplace_back(16, 300);
  seqs = decoded.TEST_GetInternalMapping();
  ASSERT_EQ(expected, seqs);
}

}  // namespace ROCKSDB_NAMESPACE


int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
