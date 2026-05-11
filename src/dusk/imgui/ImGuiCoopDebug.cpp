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

// MVP default; the lobby UI will replace this once it lands in M6.
constexpr const char* kDefaultServerUrl = "ws://localhost:7777";

std::array<char, 256>& UrlBuffer() {
    static std::array<char, 256> buf = []() {
        std::array<char, 256> b{};
        std::strncpy(b.data(), kDefaultServerUrl, b.size() - 1);
        return b;
    }();
    return buf;
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

    ImGui::SetNextWindowBgAlpha(0.65f);
    if (ImGui::Begin("Co-op Debug", nullptr, flags)) {
        const auto stats = net::GetStats();
        ImGuiStringViewText(fmt::format(FMT_STRING("State:        {}\n"), StateLabel(stats.state)));
        ImGuiStringViewText(fmt::format(FMT_STRING("Frames in:    {}\n"), stats.framesReceived));
        ImGuiStringViewText(fmt::format(FMT_STRING("Frames out:   {}\n"), stats.framesSent));
        ImGuiStringViewText(fmt::format(FMT_STRING("Frames drop:  {}\n"), stats.framesDropped));
        ImGuiStringViewText(fmt::format(FMT_STRING("Inbound q:    {}\n"), stats.inboundQueueDepth));
        ImGuiStringViewText(fmt::format(FMT_STRING("Sim ticks:    {}\n"), stats.simTicks));

        ImGui::Separator();
        auto& url = UrlBuffer();
        ImGui::InputText("URL", url.data(), url.size());

        const bool disconnected = stats.state == net::ConnectionState::Disconnected;
        ImGui::BeginDisabled(!disconnected);
        if (ImGui::Button("Connect")) {
            net::Connect(url.data());
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(disconnected);
        if (ImGui::Button("Disconnect")) {
            net::Disconnect();
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(stats.state != net::ConnectionState::Connected);
        if (ImGui::Button("Send test frame")) {
            const std::array<std::uint8_t, 4> payload = { 'P', 'I', 'N', 'G' };
            net::Send(payload);
        }
        ImGui::EndDisabled();

        ShowCornerContextMenu(m_coopDebugCorner, m_debugOverlayCorner);
    }
    ImGui::End();

    ImGui::PopFont();
}

}  // namespace dusk

#endif  // DUSK_ENABLE_MULTIPLAYER
