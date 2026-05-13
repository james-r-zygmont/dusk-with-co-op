#include "dusk/net/api_client.h"

#include "dusk/logging.h"

#include <ixwebsocket/IXHttpClient.h>

#include <nlohmann/json.hpp>

#include <cstring>

namespace dusk::net {

namespace {

using nlohmann::json;

constexpr const char* kHexLower = "0123456789abcdef";

std::string hex_encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out.push_back(kHexLower[b >> 4]);
        out.push_back(kHexLower[b & 0xF]);
    }
    return out;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode_iso(const std::string& s, std::array<std::uint8_t, kIsoHashLen>& out) {
    if (s.size() != kIsoHashLen * 2) return false;
    for (std::size_t i = 0; i < kIsoHashLen; ++i) {
        int hi = hex_nibble(s[i * 2]);
        int lo = hex_nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

ApiErrorKind classify_error_code(const std::string& code) {
    if (code == "iso_mismatch") return ApiErrorKind::IsoMismatch;
    if (code == "session_not_found") return ApiErrorKind::SessionNotFound;
    if (code == "session_full") return ApiErrorKind::SessionFull;
    if (code == "bad_request") return ApiErrorKind::BadRequest;
    return ApiErrorKind::Internal;
}

ApiError make_network_error(int status, const std::string& message) {
    ApiError err;
    err.kind = ApiErrorKind::Network;
    err.http_status = status;
    err.message = message;
    return err;
}

ApiError parse_typed_error(int status, const std::string& body) {
    ApiError err;
    err.http_status = status;
    try {
        const auto j = json::parse(body);
        const std::string error_code = j.value("error", "");
        err.kind = classify_error_code(error_code);
        err.message = j.value("message", error_code);
        if (err.kind == ApiErrorKind::IsoMismatch) {
            const std::string hex = j.value("server_hash", "");
            if (!hex_decode_iso(hex, err.server_iso_hash)) {
                err.kind = ApiErrorKind::UnexpectedResponse;
                err.message = "iso_mismatch but server_hash unparseable";
            }
        }
    } catch (const std::exception& e) {
        err.kind = ApiErrorKind::UnexpectedResponse;
        err.message = std::string("response parse: ") + e.what();
    }
    return err;
}

ix::HttpResponsePtr do_post_json(const std::string& url, const std::string& json_body) {
    ix::HttpClient client(/*async=*/ false);
    auto args = client.createRequest(url, ix::HttpClient::kPost);
    args->extraHeaders["Content-Type"] = "application/json";
    return client.post(url, json_body, args);
}

}  // namespace

ApiResult<CreateSessionResponse> ApiCreateSession(
    const std::string& base_url,
    const std::string& display_name,
    std::span<const std::uint8_t> iso_hash)
{
    if (iso_hash.size() != kIsoHashLen) {
        return ApiResult<CreateSessionResponse>::failure({
            ApiErrorKind::BadRequest, 0, "iso_hash must be 16 bytes", {}});
    }
    const std::string url = base_url + "/v1/sessions";
    const json body = {
        {"display_name", display_name},
        {"iso_hash", hex_encode(iso_hash)},
    };
    auto resp = do_post_json(url, body.dump());
    if (resp->statusCode == 0) {
        DuskLog.warn("dusk::net: CreateSession network error: {}", resp->errorMsg);
        return ApiResult<CreateSessionResponse>::failure(
            make_network_error(0, resp->errorMsg));
    }
    if (resp->statusCode != 200) {
        return ApiResult<CreateSessionResponse>::failure(
            parse_typed_error(resp->statusCode, resp->body));
    }
    try {
        const auto j = json::parse(resp->body);
        CreateSessionResponse out;
        out.session_code = j.at("session_code").get<std::string>();
        out.host_token = j.at("host_token").get<std::string>();
        out.player_index = j.at("player_index").get<std::uint8_t>();
        return ApiResult<CreateSessionResponse>::success(std::move(out));
    } catch (const std::exception& e) {
        ApiError err;
        err.kind = ApiErrorKind::UnexpectedResponse;
        err.http_status = resp->statusCode;
        err.message = std::string("body parse: ") + e.what();
        return ApiResult<CreateSessionResponse>::failure(std::move(err));
    }
}

ApiResult<JoinSessionResponse> ApiJoinSession(
    const std::string& base_url,
    const std::string& session_code,
    const std::string& display_name,
    std::span<const std::uint8_t> iso_hash)
{
    if (iso_hash.size() != kIsoHashLen) {
        return ApiResult<JoinSessionResponse>::failure({
            ApiErrorKind::BadRequest, 0, "iso_hash must be 16 bytes", {}});
    }
    const std::string url = base_url + "/v1/sessions/" + session_code + "/join";
    const json body = {
        {"display_name", display_name},
        {"iso_hash", hex_encode(iso_hash)},
    };
    auto resp = do_post_json(url, body.dump());
    if (resp->statusCode == 0) {
        DuskLog.warn("dusk::net: JoinSession network error: {}", resp->errorMsg);
        return ApiResult<JoinSessionResponse>::failure(
            make_network_error(0, resp->errorMsg));
    }
    if (resp->statusCode != 200) {
        return ApiResult<JoinSessionResponse>::failure(
            parse_typed_error(resp->statusCode, resp->body));
    }
    try {
        const auto j = json::parse(resp->body);
        JoinSessionResponse out;
        out.guest_token = j.at("guest_token").get<std::string>();
        out.player_index = j.at("player_index").get<std::uint8_t>();
        return ApiResult<JoinSessionResponse>::success(std::move(out));
    } catch (const std::exception& e) {
        ApiError err;
        err.kind = ApiErrorKind::UnexpectedResponse;
        err.http_status = resp->statusCode;
        err.message = std::string("body parse: ") + e.what();
        return ApiResult<JoinSessionResponse>::failure(std::move(err));
    }
}

}  // namespace dusk::net
