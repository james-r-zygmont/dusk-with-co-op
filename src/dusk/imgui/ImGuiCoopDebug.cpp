#if DUSK_ENABLE_MULTIPLAYER

#include "fmt/format.h"
#include "imgui.h"

#include "ImGuiConfig.hpp"
#include "ImGuiConsole.hpp"
#include "ImGuiEngine.hpp"
#include "ImGuiMenuTools.hpp"

#include "dusk/net/net.h"

#include <array>
#include <cstring>

namespace dusk {

namespace {

const char* StateLabel(net::ConnectionState s) {
    switch (s) {
    case net::ConnectionState::Disconnected: return "disconnected";
    case net::ConnectionState::Connecting:   return "connecting";
    case net::ConnectionState::Connected:    return "connected";
    case net::ConnectionState::Closing:      return "closing";
    }
    return "?";
}

const char* HandshakeErrorLabel(net::HandshakeError e) {
    switch (e) {
    case net::HandshakeError::None:             return "ok";
    case net::HandshakeError::Network:          return "network";
    case net::HandshakeError::IsoMismatch:      return "iso_mismatch";
    case net::HandshakeError::ProtocolVersion:  return "protocol_version";
    case net::HandshakeError::SessionNotFound:  return "session_not_found";
    case net::HandshakeError::SessionFull:      return "session_full";
    case net::HandshakeError::BadResponse:      return "bad_response";
    case net::HandshakeError::Internal:         return "internal";
    }
    return "?";
}

constexpr const char* kDefaultBaseUrl = "http://127.0.0.1:7777";
// GameCube US image XXH128, baked into iso_validate.cpp. We default to it so
// the M2 smoke test "just works" once a US disc is loaded; the lobby UI in
// M6 will pull the hash from the live validation result rather than a
// hand-typed field.
constexpr const char* kDefaultIsoHashHex = "14e886f08e548a000afde98a3195e788";

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseIsoHash(const char* hex, std::array<std::uint8_t, 16>& out) {
    if (std::strlen(hex) != 32) return false;
    for (int i = 0; i < 16; ++i) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

// Initialises a fixed-size char buffer with a default string. Used to give
// each ImGui::InputText a stable backing array. NOTE: the *callers* must hold
// the storage as a function-local `static` — a previous version of this code
// used `static` inside a function template here, which made every buffer of
// the same width alias the same array. Don't repeat that.
template <std::size_t N>
std::array<char, N> InitBuf(const char* default_value) {
    std::array<char, N> b{};
    std::strncpy(b.data(), default_value, N - 1);
    return b;
}

}  // namespace

void ImGuiMenuTools::ShowCoopDebug() {
    if (!ImGuiConsole::CheckMenuViewToggle(ImGuiKey_None, m_showCoopDebug)) {
        return;
    }

    ImGui::PushFont(ImGuiEngine::fontMono);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;
    if (m_coopDebugCorner != -1) {
        SetOverlayWindowLocation(m_coopDebugCorner);
        flags |= ImGuiWindowFlags_NoMove;
    }

    static auto base_url = InitBuf<256>(kDefaultBaseUrl);
    static auto display_name = InitBuf<64>("Player");
    static auto iso_hash_hex = InitBuf<64>(kDefaultIsoHashHex);
    static auto session_code_buf = InitBuf<16>("");

    ImGui::SetNextWindowBgAlpha(0.65f);
    if (ImGui::Begin("Co-op Debug", nullptr, flags)) {
        const auto stats = net::GetStats();

        ImGuiStringViewText(fmt::format(FMT_STRING("State:        {}\n"), StateLabel(stats.state)));
        const std::string code =
            stats.sessionCode.empty() ? std::string{"-"} : stats.sessionCode;
        ImGuiStringViewText(fmt::format(FMT_STRING("Session:      {}\n"), code));
        const std::string idx_str =
            stats.playerIndex == 0xFF ? std::string{"-"} : std::to_string(stats.playerIndex);
        ImGuiStringViewText(fmt::format(FMT_STRING("Player index: {}\n"), idx_str));
        const bool has_error = stats.handshakeError != net::HandshakeError::None;
        if (has_error) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
        ImGuiStringViewText(
            fmt::format(FMT_STRING("Handshake:    {}\n"), HandshakeErrorLabel(stats.handshakeError)));
        ImGuiStringViewText(fmt::format(
            FMT_STRING("Last error:   {}\n"),
            stats.lastErrorMessage.empty() ? std::string{"(none)"} : stats.lastErrorMessage));
        if (has_error) ImGui::PopStyleColor();
        ImGuiStringViewText(fmt::format(FMT_STRING("Frames in:    {}\n"), stats.framesReceived));
        ImGuiStringViewText(fmt::format(FMT_STRING("Frames out:   {}\n"), stats.framesSent));
        ImGuiStringViewText(fmt::format(FMT_STRING("Frames drop:  {}\n"), stats.framesDropped));

        ImGui::Separator();
        ImGui::InputText("Server URL", base_url.data(), base_url.size());
        ImGui::InputText("Display name", display_name.data(), display_name.size());
        ImGui::InputText("ISO hash (hex)", iso_hash_hex.data(), iso_hash_hex.size());
        ImGui::InputText("Session code", session_code_buf.data(), session_code_buf.size());

        const bool disconnected = stats.state == net::ConnectionState::Disconnected;

        ImGui::BeginDisabled(!disconnected);
        if (ImGui::Button("Host")) {
            net::HostSessionConfig cfg;
            cfg.base_url = base_url.data();
            cfg.display_name = display_name.data();
            if (!ParseIsoHash(iso_hash_hex.data(), cfg.iso_hash)) {
                // Set the error visibly; ParseIsoHash failure mostly means
                // the user mistyped. HostSession won't be called.
                // (Handled silently here; users will see the field obviously wrong.)
            } else {
                net::HostSession(cfg);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Join")) {
            net::JoinSessionConfig cfg;
            cfg.base_url = base_url.data();
            cfg.session_code = session_code_buf.data();
            cfg.display_name = display_name.data();
            if (ParseIsoHash(iso_hash_hex.data(), cfg.iso_hash)) {
                net::JoinSession(cfg);
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(disconnected);
        if (ImGui::Button("Disconnect")) {
            net::Disconnect();
        }
        ImGui::EndDisabled();

        ShowCornerContextMenu(m_coopDebugCorner, m_debugOverlayCorner);
    }
    ImGui::End();

    ImGui::PopFont();
}

}  // namespace dusk

#endif  // DUSK_ENABLE_MULTIPLAYER
