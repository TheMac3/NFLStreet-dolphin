// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Socket-free, one-shot GN7E69 revision-0 menu navigation. Only observed guest
// screens and emulated retraces advance the script; host time and local pads
// never do. Requalify the route and bump VERSION for another image/menu layout.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace NSRQuickstart
{
constexpr std::string_view VERSION = "1";
enum class Mode { Manual, QuickPlay, Pickup };

// Strict parsing prevents a misspelled room selection becoming manual play.
inline std::optional<Mode> Parse(std::string_view id)
{
  if (id.empty() || id == "manual") return Mode::Manual;
  if (id == "quick-play") return Mode::QuickPlay;
  if (id == "pickup") return Mode::Pickup;
  return std::nullopt;
}

class Navigator
{
public:
  // Start disabled for manual sessions; the owner constructs once per boot.
  Navigator() = default;
  explicit Navigator(Mode mode) : m_mode(mode) {}
  // Completed navigation never re-arms on a later match or menu return.
  bool Active() const { return m_mode != Mode::Manual && !m_done; }
  bool Failed() const { return m_failed; }
  std::uint32_t StepIndex() const { return m_step; }

  // Returns raw GC buttons for all four pads; nullopt means humans own input.
  // Duplicate polls for one retrace must neither advance nor repeat an edge.
  std::optional<std::array<std::uint16_t, 4>> Poll(std::uint32_t frame,
                                                 std::uint32_t screen)
  {
    if (m_last_frame && frame == *m_last_frame) return m_cached;
    m_last_frame = frame;
    if (!Active()) return m_cached = std::nullopt;
    std::array<std::uint16_t, 4> buttons{};
    if (!m_started) { m_started = frame; }
    if (frame - *m_started > 18000) m_failed = true;
    if (m_failed) return m_cached = buttons;  // caller stops the session

    const bool pickup = m_mode == Mode::Pickup;
    // The command-stack screen IDs were observed with the raw-pad hook on
    // blank cards: title, training, help, modes, controllers, teams/draft.
    const std::array<Step, 10> steps{{
      {0x00000003, 0x1000, 1},
      {0x00010003, 0x0004, 1}, {0x00010003, 0x0004, 1},
      {0x00010003, 0x0100, 1},
      {0x00000001, static_cast<std::uint16_t>(pickup ? 0 : 0x0200), 1},
      {0x00000001, static_cast<std::uint16_t>(pickup ? 0x0008 : 0x0001), 1},
      {0x00000001, 0x0100, 1}, {0x00000011, 0x0002, 14},
      {0x00000011, 0x0100, 15}, {pickup ? 0x0000000fU : 0x00000007U, 0, 0}
    }};
    const Step& step = steps[m_step];
    if (!m_pulse)
    {
      if (screen != step.screen) { m_screen_since.reset(); }
      else if (!m_screen_since) { m_screen_since = frame; }
      else if (frame - *m_screen_since >= 60)
      {
        if (step.mask == 0) { m_done = true; }
        else { m_pulse = frame; }
      }
    }
    if (m_pulse)
    {
      const auto elapsed = frame - *m_pulse;
      // Release before the next action, even if a transition has already
      // started. Stable-screen waiting also covers the game's input debounce.
      if (elapsed < 6 && screen == step.screen)
        for (std::uint32_t port = 0; port < 4; ++port)
          if (step.mask & (1U << port)) buttons[port] = step.buttons;
      if (elapsed >= 60)
      {
        // Title animation can expose its screen before accepting START.
        // Retry only this idempotent action while still on the title screen.
        if (m_step != 0 || screen != step.screen)
          ++m_step;
        m_pulse.reset();
        m_screen_since.reset();
      }
    }
    return m_cached = buttons;
  }

private:
  struct Step { std::uint32_t screen; std::uint16_t buttons; std::uint16_t mask; };
  Mode m_mode = Mode::Manual;
  bool m_done = false;
  bool m_failed = false;
  std::uint32_t m_step = 0;
  std::optional<std::uint32_t> m_started, m_last_frame, m_screen_since, m_pulse;
  std::optional<std::array<std::uint16_t, 4>> m_cached;
};
}  // namespace NSRQuickstart
