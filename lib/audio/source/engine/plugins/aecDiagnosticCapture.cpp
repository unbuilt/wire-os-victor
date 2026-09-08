#include "audioEngine/plugins/aecDiagnosticCapture.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Anki { namespace AudioEngine { namespace PlugIns {
namespace {
const char* const files[] = {"raw.wav", "reference.wav", "mic.csv", "reference.csv", "clocks.csv", "manifest.json", "manifest.part"};
const char* traceHeader = "observed_ns,predicted_ns,predicted_before_ns,elapsed_ns,duration_ns,residual_ns,min_ns,min_at_ns,max_ns,window_start_ns,span_ns,previous_min_ns,rate_before_ppb,rate_after_ppb,windows,updates,reason,closed,ready";
void TraceRow(std::ostream& out, const AecClockTrace& t)
{
  out << t.observedNs << ',' << t.predictedNs << ',' << t.predictedBeforeNs << ',' << t.elapsedNs << ',' << t.durationNs << ','
      << t.residualNs << ',' << t.minimumNs << ',' << t.minimumAtNs << ',' << t.maximumNs << ','
      << t.windowStartNs << ',' << t.spanNs << ',' << t.previousMinimumNs << ','
      << t.rateBeforePpb << ',' << t.rateAfterPpb << ',' << t.windows << ',' << t.updates << ','
      << t.reason << ',' << t.closed << ',' << t.ready << '\n';
}
bool Finish(std::ofstream& out)
{
  out.flush();
  const bool good = bool(out);
  out.close();
  return good && !out.fail();
}
std::array<unsigned char, 44> WaveHeader(uint32_t bytes, unsigned channels, unsigned rate)
{
  std::array<unsigned char, 44> header{};
  auto u16 = [&header](size_t offset, uint16_t value) {
    header[offset] = value & 255; header[offset + 1] = value >> 8;
  };
  auto u32 = [&u16](size_t offset, uint32_t value) {
    u16(offset, value & 65535); u16(offset + 2, value >> 16);
  };
  std::memcpy(header.data(), "RIFF", 4); u32(4, 36 + bytes);
  std::memcpy(header.data() + 8, "WAVEfmt ", 8); u32(16, 16);
  u16(20, 1); u16(22, channels); u32(24, rate);
  u32(28, rate * channels * 2); u16(32, channels * 2); u16(34, 16);
  std::memcpy(header.data() + 36, "data", 4); u32(40, bytes);
  return header;
}
uint64_t HashBytes(uint64_t hash, const unsigned char* data, size_t bytes)
{
  for (size_t i = 0; i < bytes; ++i) { hash = (hash ^ data[i]) * 1099511628211ULL; }
  return hash;
}
}

int64_t AecDiagnosticCapture::NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

AecDiagnosticCapture::~AecDiagnosticCapture() { Shutdown(); }

bool AecDiagnosticCapture::Busy() const
{
  const auto state = _state.load();
  return state == Active || state == Writing;
}

void AecDiagnosticCapture::Allocate(Lane& lane, size_t samples, size_t records)
{
  lane.sampleCapacity = samples;
  lane.recordCapacity = records;
  lane.pcm.reset(new int16_t[samples]());
  lane.records.reset(new Record[records]());
}

bool AecDiagnosticCapture::Begin(const std::string& directory, unsigned seconds, int mode,
                                 int micAgeMs, int refDelayMs)
{
  std::lock_guard<std::mutex> lock(_control);
  if (_shutdown || Busy() || seconds < 1 || seconds > 15 || mode != 1 ||
      micAgeMs < 0 || micAgeMs > 200 || refDelayMs < 0 || refDelayMs > 200) { return false; }
  if (_worker.joinable()) { _worker.join(); }
  _id = "trial_" + std::to_string(getpid()) + "_" + std::to_string(NowNs());
  _path = directory + "/" + _id;
  _message.clear();
  _errors.store(0);
  _cancel.store(false);
  _seconds = seconds;
  _micAgeMs = micAgeMs;
  _refDelayMs = refDelayMs;
  try {
    _message.reserve(128);
    if (!PrepareDirectory(directory, _path)) { throw std::runtime_error("directory/rotation failed"); }
    _buffer.reset(new Buffer);
    Allocate(_buffer->ref, (seconds + 1) * 32000, (seconds + 1) * 128);
    Allocate(_buffer->mic, (seconds + 1) * 128 * 640, (seconds + 1) * 128);
    _startNs = NowNs();
    _endNs = _startNs + seconds * 1000000000LL;
    _state.store(Active);
    _worker = std::thread(&AecDiagnosticCapture::Worker, this);
    return true;
  } catch (const std::exception& error) {
    _state.store(Writing);
    while (_writers.load()) { std::this_thread::yield(); }
    _buffer.reset();
    _message = error.what();
    _state.store(Failed);
    return false;
  }
}

void AecDiagnosticCapture::Shutdown()
{
  {
    std::lock_guard<std::mutex> lock(_control);
    _shutdown = true;
    _cancel.store(true);
  }
  if (_worker.joinable()) { _worker.join(); }
}

bool AecDiagnosticCapture::Enter(int64_t receivedNs)
{
  if (_state.load() != Active) { return false; }
  _writers.fetch_add(1);
  // Stop publishes Writing before waiting for zero writers. A late entrant
  // cannot dereference the buffer, even if the worker has already freed it.
  if (_state.load() != Active) { Leave(); return false; }
  if (receivedNs && (receivedNs < _startNs || receivedNs >= _endNs)) { Leave(); return false; }
  return true;
}

void AecDiagnosticCapture::NoteError(uint32_t error)
{
  const auto receivedNs = NowNs();
  if (_errors.load() & error) { return; }
  if (!Enter(receivedNs)) { return; }
  _errors.fetch_or(error);
  Leave();
}

void AecDiagnosticCapture::Append(Lane& lane, const int16_t* pcm, uint32_t count, unsigned channels,
                                 uint64_t index, int64_t receivedNs, uint32_t first, uint32_t last,
                                 bool valid, const AecCalibratedClock& clock,
                                 uint32_t sinkErrors, uint32_t ringDrops)
{
  if (!pcm || count == 0 || count > 32000) { _errors.fetch_or(InvalidFormat); return; }
  const auto observation = clock.Trace().observedNs;
  if (!valid || observation <= 0 ||
      observation > std::numeric_limits<int64_t>::max() - 1209600000000000LL ||
      (channels == 4 && observation > receivedNs)) { _errors.fetch_or(InvalidObservation); }
  if (lane.count == lane.recordCapacity || size_t(count) * channels > lane.sampleCapacity - lane.samples) {
    _errors.fetch_or(Overflow);
    return;
  }
  if (lane.count == 0) {
    lane.historyCount = clock.CopyHistory(lane.history.data(), lane.history.size());
    lane.firstFault = clock.FirstFaultTrace();
  } else {
    const auto& previous = lane.records[lane.count - 1];
    if (index != previous.index + previous.count ||
        (channels == 4 && first - previous.last != 1u)) { _errors.fetch_or(SourceGap); }
  }
  if (channels == 4 && (!valid || last - first != 1u)) { _errors.fetch_or(SourceGap); }
  auto& record = lane.records[lane.count++];
  record.index = index; record.offset = lane.samples / channels;
  record.receivedNs = receivedNs; record.count = count;
  record.first = first; record.last = last; record.valid = valid;
  record.sinkErrors = sinkErrors; record.ringDrops = ringDrops;
  record.clock = channels == 1 && !valid ? AecClockTrace{} : clock.Trace();
  std::memcpy(lane.pcm.get() + lane.samples, pcm, size_t(count) * channels * sizeof(int16_t));
  lane.samples += size_t(count) * channels;
}

void AecDiagnosticCapture::Reference(const int16_t* pcm, uint32_t count, uint64_t index,
                                    int64_t receivedNs, const AecCalibratedClock& clock,
                                    uint32_t sinkErrors, uint32_t ringDrops, bool observationValid)
{
  if (!Enter(receivedNs)) { return; }
  Append(_buffer->ref, pcm, count, 1, index, receivedNs, 0, 0, observationValid, clock, sinkErrors, ringDrops);
  Leave();
}

void AecDiagnosticCapture::Microphone(const int16_t* pcm, uint64_t index, int64_t receivedNs,
                                     uint32_t first, uint32_t last, bool valid,
                                     const AecCalibratedClock& clock)
{
  if (!Enter(receivedNs)) { return; }
  Append(_buffer->mic, pcm, 160, 4, index, receivedNs, first, last, valid, clock, 0, 0);
  Leave();
}

void AecDiagnosticCapture::Worker()
{
  while (!_cancel.load() && NowNs() < _endNs) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
  _state.store(Writing);
  while (_writers.load()) { std::this_thread::yield(); }
  if (_cancel.load()) { _errors.fetch_or(Aborted); }
  if (!_buffer->mic.count || !_buffer->ref.count) { _errors.fetch_or(MissingStream); }
  bool saved = false;
  try { saved = WriteFiles(); } catch (...) { saved = false; }
  _buffer.reset();
  {
    std::lock_guard<std::mutex> lock(_control);
    _message = saved ? "artifacts committed; diagnostic only, not an AEC comparison" : "artifact write failed; no complete trial";
    _state.store(saved ? Saved : Failed);
  }
}

bool AecDiagnosticCapture::PrepareDirectory(const std::string& directory, const std::string& trial)
{
  if (mkdir(directory.c_str(), 0700) && errno != EEXIST) { return false; }
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(directory.c_str()), &closedir);
  if (!dir) { return false; }
  std::vector<std::pair<time_t, std::string>> previous;
  while (const auto* entry = readdir(dir.get())) {
    const std::string name(entry->d_name);
    if (name.compare(0, 6, "trial_") != 0 || name.find_first_not_of("0123456789_", 6) != std::string::npos) { continue; }
    struct stat info{};
    const auto path = directory + "/" + name;
    if (!lstat(path.c_str(), &info) && S_ISDIR(info.st_mode)) { previous.emplace_back(info.st_mtime, path); }
  }
  dir.reset();
  std::sort(previous.begin(), previous.end());
  while (previous.size() >= 100) {
    std::unique_ptr<DIR, decltype(&closedir)> old(opendir(previous.front().second.c_str()), &closedir);
    if (!old) { return false; }
    bool ownedContents = true;
    while (const auto* entry = readdir(old.get())) {
      const std::string name(entry->d_name);
      if (name == "." || name == "..") { continue; }
      bool known = false;
      for (const auto* file : files) { known |= name == file; }
      struct stat info{};
      const auto path = previous.front().second + "/" + name;
      if (!known || lstat(path.c_str(), &info) || !S_ISREG(info.st_mode)) { ownedContents = false; }
    }
    old.reset();
    if (!ownedContents) { return false; }
    for (const auto* file : files) {
      const auto path = previous.front().second + "/" + file;
      if (unlink(path.c_str()) && errno != ENOENT) { return false; }
    }
    // Unknown contents are never recursively removed.
    if (rmdir(previous.front().second.c_str())) { return false; }
    previous.erase(previous.begin());
  }
  return mkdir(trial.c_str(), 0700) == 0;
}

bool AecDiagnosticCapture::SyncFile(const std::string& path)
{
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) { return false; }
  const bool result = fsync(fd) == 0;
  return close(fd) == 0 && result;
}

bool AecDiagnosticCapture::WriteAll(int fd, const void* data, size_t bytes, WriteFunction writer)
{
  if (!writer) { writer = &::write; }
  const auto* next = static_cast<const unsigned char*>(data);
  while (bytes) {
    const auto written = writer(fd, next, bytes);
    if (written < 0 && errno == EINTR) { continue; }
    if (written <= 0 || static_cast<size_t>(written) > bytes) { return false; }
    next += written;
    bytes -= static_cast<size_t>(written);
  }
  return true;
}

bool AecDiagnosticCapture::ValidateWave(const std::string& path, size_t samples,
                                      unsigned channels, unsigned rate)
{
  if ((channels != 1 && channels != 4) || samples % channels ||
      samples > (std::numeric_limits<uint32_t>::max() - 44u) / 2u) { return false; }
  const auto expected = WaveHeader(static_cast<uint32_t>(samples * 2), channels, rate);
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) { return false; }
  struct stat info{};
  bool good = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
    static_cast<uint64_t>(info.st_size) == expected.size() + uint64_t(samples) * 2;
  std::array<unsigned char, 44> actual{};
  size_t received = 0;
  while (good && received < actual.size()) {
    const auto count = read(fd, actual.data() + received, actual.size() - received);
    if (count < 0 && errno == EINTR) { continue; }
    if (count <= 0) { good = false; break; }
    received += static_cast<size_t>(count);
  }
  if (close(fd)) { good = false; }
  return good && actual == expected;
}

bool AecDiagnosticCapture::WriteWave(const std::string& path, const Lane& lane,
                                   unsigned channels, unsigned rate, uint64_t* expectedHash)
{
  if ((channels != 1 && channels != 4) || lane.samples % channels ||
      lane.samples > (std::numeric_limits<uint32_t>::max() - 44u) / 2u ||
      (lane.samples && !lane.pcm) ||
      (channels == 4 && (lane.samples % 640 || lane.count != lane.samples / 640))) { return false; }
  const auto header = WaveHeader(static_cast<uint32_t>(lane.samples * 2), channels, rate);
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) { return false; }
  uint64_t hash = HashBytes(14695981039346656037ULL, header.data(), header.size());
  bool good = WriteAll(fd, header.data(), header.size());
  // Never pass arbitrary PCM bytes to ostream::put(char): the deployed ARM
  // libc++ and -fsigned-char callers disagree on char argument extension.
  // At stream overflow, 0xff can become EOF and disappear without an error.
  std::array<unsigned char, 4096> chunk{};
  size_t used = 0;
  for (size_t i = 0; good && i < lane.samples; ++i) {
    const size_t source = channels == 1 ? i : (i / 640) * 640 + (i % 4) * 160 + (i % 640) / 4;
    const auto value = static_cast<uint16_t>(lane.pcm[source]);
    chunk[used++] = value & 255;
    chunk[used++] = value >> 8;
    if (used == chunk.size()) {
      hash = HashBytes(hash, chunk.data(), used);
      good = WriteAll(fd, chunk.data(), used);
      used = 0;
    }
  }
  if (good && used) {
    hash = HashBytes(hash, chunk.data(), used);
    good = WriteAll(fd, chunk.data(), used);
  }
  if (good && fsync(fd)) { good = false; }
  if (close(fd)) { good = false; }
  if (good) { good = ValidateWave(path, lane.samples, channels, rate); }
  if (good && expectedHash) { *expectedHash = hash; }
  return good;
}

bool AecDiagnosticCapture::WriteRecords(const std::string& path, const Lane& lane, const std::string& id)
{
  std::ofstream out(path);
  out << "trial,index,file_offset,count,received_ns,source_first,source_last,source_valid,sink_errors,ring_drops," << traceHeader << '\n';
  for (size_t i = 0; i < lane.count; ++i) {
    const auto& r = lane.records[i];
    out << id << ',' << r.index << ',' << r.offset << ',' << r.count << ',' << r.receivedNs << ','
        << r.first << ',' << r.last << ',' << r.valid << ',' << r.sinkErrors << ',' << r.ringDrops << ',';
    TraceRow(out, r.clock);
  }
  return Finish(out) && SyncFile(path);
}

bool AecDiagnosticCapture::WriteHistory(const std::string& path, const Buffer& buffer, const std::string& id)
{
  std::ofstream out(path);
  out << "trial,stream," << traceHeader << '\n';
  for (const auto& item : {std::make_pair("mic", &buffer.mic), std::make_pair("reference", &buffer.ref)}) {
    for (size_t i = 0; i < item.second->historyCount; ++i) {
      out << id << ',' << item.first << ',';
      TraceRow(out, item.second->history[i]);
    }
    if (item.second->firstFault.reason) {
      out << id << ',' << item.first << "_first_fault,";
      TraceRow(out, item.second->firstFault);
    }
  }
  return Finish(out) && SyncFile(path);
}

bool AecDiagnosticCapture::FileMetadata(const std::string& path, std::string& metadata,
                                      uint64_t expectedBytes, const uint64_t* expectedHash)
{
  std::ifstream in(path, std::ios::binary);
  uint64_t hash = 14695981039346656037ULL, bytes = 0;
  char chunk[4096];
  while (in) {
    in.read(chunk, sizeof(chunk));
    const auto count = in.gcount();
    for (std::streamsize i = 0; i < count; ++i) { hash = (hash ^ static_cast<unsigned char>(chunk[i])) * 1099511628211ULL; }
    bytes += count;
  }
  if (in.bad() || !in.eof() || (expectedBytes && bytes != expectedBytes) ||
      (expectedHash && hash != *expectedHash)) { return false; }
  std::ostringstream out;
  out << "{\"bytes\":" << bytes << ",\"fnv1a64\":\"" << std::hex << std::setw(16) << std::setfill('0') << hash << "\"}";
  if (!out) { return false; }
  metadata = out.str();
  return true;
}

bool AecDiagnosticCapture::WriteFiles()
{
  uint64_t waveHashes[2]{};
  if (!WriteWave(_path + "/raw.wav", _buffer->mic, 4, 15625, &waveHashes[0]) ||
      !WriteWave(_path + "/reference.wav", _buffer->ref, 1, 32000, &waveHashes[1]) ||
      !WriteRecords(_path + "/mic.csv", _buffer->mic, _id) ||
      !WriteRecords(_path + "/reference.csv", _buffer->ref, _id) ||
      !WriteHistory(_path + "/clocks.csv", *_buffer, _id)) { return false; }
  std::array<std::string, 5> metadata;
  for (size_t i = 0; i < metadata.size(); ++i) {
    uint64_t expectedBytes = 0;
    if (i < 2) {
      const auto& lane = i == 0 ? _buffer->mic : _buffer->ref;
      if (!ValidateWave(_path + "/" + files[i], lane.samples, i == 0 ? 4 : 1,
                        i == 0 ? 15625 : 32000)) { return false; }
      expectedBytes = 44 + uint64_t(lane.samples) * 2;
    }
    if (!FileMetadata(_path + "/" + files[i], metadata[i], expectedBytes,
                      i < 2 ? &waveHashes[i] : nullptr)) { return false; }
  }
  std::ofstream manifest(_path + "/manifest.part");
  manifest << "{\"schema\":1,\"revision\":110,\"trial\":\"" << _id
           << "\",\"mode\":\"reference\",\"cancel_enabled\":false,\"adaptation_enabled\":false,"
           << "\"write_complete\":true,\"diagnostic_only\":true,\"errors\":" << _errors.load()
           << ",\"start_ns\":" << _startNs << ",\"end_ns\":" << _endNs << ",\"seconds\":" << _seconds
           << ",\"mic_age_ms\":" << _micAgeMs << ",\"ref_delay_ms\":" << _refDelayMs
           << ",\"mic_frames\":" << _buffer->mic.samples / 4 << ",\"reference_frames\":" << _buffer->ref.samples
           << ",\"mic_records\":" << _buffer->mic.count << ",\"reference_records\":" << _buffer->ref.count
           << ",\"mic_history_records\":" << _buffer->mic.historyCount
           << ",\"reference_history_records\":" << _buffer->ref.historyCount << ",\"files\":{";
  for (size_t i = 0; i < 5; ++i) {
    if (i) { manifest << ','; }
    manifest << '"' << files[i] << "\":" << metadata[i];
  }
  manifest << "}}\n";
  if (!Finish(manifest) || !SyncFile(_path + "/manifest.part") ||
      rename((_path + "/manifest.part").c_str(), (_path + "/manifest.json").c_str())) { return false; }
  if (!SyncFile(_path)) { unlink((_path + "/manifest.json").c_str()); return false; }
  return true;
}

std::string AecDiagnosticCapture::Escape(const std::string& value)
{
  std::string result;
  for (char ch : value) { if (ch == '"' || ch == '\\') { result += '\\'; } result += ch; }
  return result;
}

std::string AecDiagnosticCapture::StatusJson(int mode) const
{
  std::lock_guard<std::mutex> lock(_control);
  static const char* names[] = {"idle", "active", "writing", "saved", "failed"};
  std::string version;
  std::ifstream("/etc/os-version") >> version;
  std::ostringstream out;
  const auto state = _state.load();
  const auto errors = _errors.load();
  out << "{\"revision\":110,\"schema\":1,\"mode\":" << mode << ",\"os_version\":\"" << Escape(version)
      << "\",\"state\":\"" << (state == Saved && errors ? "invalid" : names[state])
      << "\",\"write_complete\":" << (state == Saved ? "true" : "false") << ",\"trial\":\"" << Escape(_id)
      << "\",\"path\":\"" << Escape(_path) << "\",\"errors\":" << errors
      << ",\"message\":\"" << Escape(_message) << "\"}";
  return out.str();
}

}}}
