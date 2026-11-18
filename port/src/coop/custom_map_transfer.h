#pragma once
#include "gc/sha256.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace port::coop {
inline constexpr uint64_t custom_map_max_bytes = 128ull * 1024 * 1024;
inline constexpr uint64_t custom_preview_max_bytes = 4ull * 1024 * 1024;
inline constexpr unsigned custom_map_chunk_bytes = 32 * 1024;

struct CustomMapRequest {
    uint64_t epoch = 0;
    std::string alias, id, map_sha256, known_package_sha256;
    bool operator==(const CustomMapRequest&) const = default;
};
struct CustomMapManifest {
    std::string id, alias, title, base_level, map_sha256, preview_sha256, package_sha256;
    uint32_t modes = 0;
    uint64_t map_bytes = 0, preview_bytes = 0;
    bool operator==(const CustomMapManifest&) const = default;
};
// Network code retains immutable payloads prepared by a caller-owned I/O job.
// Only these three fixed roles are transferred; no peer supplies a file path.
struct CustomMapPackage {
    CustomMapManifest manifest;
    std::shared_ptr<const std::vector<uint8_t>> map, preview;
};
enum class CustomMapTransferPhase { Idle, Requesting, Receiving, Verifying, Complete, Cancelled, Failed };
struct CustomMapTransferStatus {
    CustomMapTransferPhase phase = CustomMapTransferPhase::Idle;
    CustomMapRequest request;
    CustomMapManifest manifest;
    uint64_t received = 0, total = 0;
    std::string error;
};
struct CustomMapDownload {
    CustomMapRequest request;
    std::shared_ptr<const CustomMapPackage> package;
    bool cache_hit = false;
};

bool custom_map_request_valid(const CustomMapRequest&, std::string& error);
bool custom_map_manifest_valid(const CustomMapManifest&, std::string& error);
// Metadata/length validation is cheap. Set verify_hashes in an I/O worker when
// preparing an offer; receivers independently verify full payload digests.
bool custom_map_package_valid(const CustomMapPackage&, std::string& error, bool verify_hashes = true);
bool custom_map_request_matches(const CustomMapRequest&, const CustomMapManifest&);
std::string custom_map_package_fingerprint(const CustomMapManifest&);
std::vector<uint8_t> encode_custom_map_manifest(const CustomMapManifest&);
bool decode_custom_map_manifest(std::span<const uint8_t>, CustomMapManifest&, std::string& error);

// One bounded, sequential receiver. It never opens files or mutates package
// folders; callers install a completed, digest-verified result asynchronously.
class CustomMapReceiver {
public:
    bool request(uint64_t token, const CustomMapRequest&, std::string& error);
    bool offer(uint64_t token, uint64_t epoch, const CustomMapManifest&, std::string& error);
    bool chunk(uint64_t token, uint64_t epoch, uint64_t offset, std::span<const uint8_t>, std::string& error);
    bool finish(uint64_t token, uint64_t epoch, std::string& error, bool cached = false);
    void fail(const std::string&);
    void cancel(const std::string& reason = {});
    const CustomMapTransferStatus& status() const { return status_; }
    uint64_t token() const { return token_; }
    std::optional<CustomMapDownload> take();
private:
    uint64_t token_ = 0;
    CustomMapTransferStatus status_;
    std::shared_ptr<std::vector<uint8_t>> map_, preview_;
    std::shared_ptr<const CustomMapPackage> complete_;
    gc::Sha256 map_digest_, preview_digest_;
    void release();
};
} // namespace port::coop
