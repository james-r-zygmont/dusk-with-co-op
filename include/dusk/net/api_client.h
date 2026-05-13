#ifndef DUSK_NET_API_CLIENT_H
#define DUSK_NET_API_CLIENT_H

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

namespace dusk::net {

constexpr std::size_t kIsoHashLen = 16;

struct CreateSessionResponse {
    std::string session_code;
    std::string host_token;
    std::uint8_t player_index;
};

struct JoinSessionResponse {
    std::string guest_token;
    std::uint8_t player_index;
};

enum class ApiErrorKind {
    Network,            // Couldn't reach the server / TCP failure.
    UnexpectedResponse, // Server returned something we couldn't parse.
    BadRequest,         // 400 from server with structured error.
    SessionNotFound,    // 404.
    SessionFull,        // 409 session_full.
    IsoMismatch,        // 409 iso_mismatch (server_iso_hash filled).
    Internal,           // 500.
};

struct ApiError {
    ApiErrorKind kind = ApiErrorKind::Internal;
    int http_status = 0;
    std::string message;
    std::array<std::uint8_t, kIsoHashLen> server_iso_hash{};  // for IsoMismatch
};

template <typename T>
class ApiResult {
public:
    static ApiResult success(T value) { return ApiResult{std::move(value)}; }
    static ApiResult failure(ApiError err) { return ApiResult{std::move(err)}; }

    bool ok() const { return std::holds_alternative<T>(state_); }
    const T& value() const { return std::get<T>(state_); }
    T&& take_value() { return std::move(std::get<T>(state_)); }
    const ApiError& error() const { return std::get<ApiError>(state_); }

private:
    explicit ApiResult(T v) : state_(std::move(v)) {}
    explicit ApiResult(ApiError e) : state_(std::move(e)) {}
    std::variant<T, ApiError> state_;
};

// Synchronous. Call from a worker thread (the M1 net worker is fine) so the
// UI thread doesn't block on HTTP. `base_url` should be `http://host:port`
// without a trailing slash.
//
// Prefix is `Api` to disambiguate from the higher-level lobby helpers in
// `dusk/net/net.h` (`HostSession` / `JoinSession`) which wrap these.
ApiResult<CreateSessionResponse> ApiCreateSession(
    const std::string& base_url,
    const std::string& display_name,
    std::span<const std::uint8_t> iso_hash);

ApiResult<JoinSessionResponse> ApiJoinSession(
    const std::string& base_url,
    const std::string& session_code,
    const std::string& display_name,
    std::span<const std::uint8_t> iso_hash);

}  // namespace dusk::net

#endif
