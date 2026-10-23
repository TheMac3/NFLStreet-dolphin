// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// TEST-ONLY virtual SDL controllers; see NSRTestVirtualPads.h. Inert unless
// NSR_TEST_VIRTUAL_PADS is set.

#include "InputCommon/ControllerInterface/SDL/NSRTestVirtualPads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/VideoInterface.h"
#include "Core/System.h"

namespace ciface::SDL::NSRTestVirtualPads
{
namespace
{
struct Kind
{
  const char* key;
  const char* name;  // what SDL's HIDAPI/XInput drivers call the real pad
  Uint16 vendor;
  Uint16 product;
  bool touchpad;
};

constexpr std::array<Kind, 4> KINDS{{
    {"ps4", "PS4 Controller", 0x054c, 0x09cc, true},
    {"ps5", "PS5 Controller", 0x054c, 0x0ce6, true},
    {"xbox360", "Xbox 360 Controller", 0x045e, 0x028e, false},
    {"xboxone", "Xbox One Controller", 0x045e, 0x02ea, false},
}};

// One scripted state: SDL gamepad button bits, then LX LY RX RY (-127..127, +Y up).
struct Step
{
  u32 until = 0;
  u32 buttons = 0;  // bit i = SDL_GamepadButton i
  bool l2 = false, r2 = false;
  std::array<int, 4> sticks{};
  bool operator==(const Step& o) const
  {
    return buttons == o.buttons && l2 == o.l2 && r2 == o.r2 && sticks == o.sticks;
  }
};

struct Pad
{
  const Kind* kind = nullptr;
  SDL_JoystickID id = 0;
  SDL_Joystick* joystick = nullptr;
  u32 button_mask = 0;  // SDL_GamepadButton bits exposed (virtual button i = i-th set bit)
};

std::mutex s_mutex;
std::vector<Pad> s_pads;  // attached pads, in NSR_TEST_VIRTUAL_PADS order once all are in
std::vector<int> s_order;  // NSR_TEST_VIRTUAL_PADS index of each s_pads entry
std::vector<Step> s_script;
u32 s_loop_start = 0, s_loop_len = 0;
int s_script_pad = 0;
Step s_applied;
bool s_have_applied = false;
std::vector<std::thread> s_late;
std::atomic<bool> s_stop{false};
FILE* s_log = nullptr;

void Note(const std::string& line)
{
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "NSR test pad: {}", line);
  if (s_log)
  {
    std::fprintf(s_log, "%s\n", line.c_str());
    std::fflush(s_log);
  }
}

u32 ButtonBit(std::string_view n)
{
  struct Name
  {
    std::string_view name;
    int button;
  };
  static constexpr std::array<Name, 26> NAMES{{
      {"s", SDL_GAMEPAD_BUTTON_SOUTH},        {"cross", SDL_GAMEPAD_BUTTON_SOUTH},
      {"e", SDL_GAMEPAD_BUTTON_EAST},         {"circle", SDL_GAMEPAD_BUTTON_EAST},
      {"w", SDL_GAMEPAD_BUTTON_WEST},         {"square", SDL_GAMEPAD_BUTTON_WEST},
      {"n", SDL_GAMEPAD_BUTTON_NORTH},        {"triangle", SDL_GAMEPAD_BUTTON_NORTH},
      {"back", SDL_GAMEPAD_BUTTON_BACK},      {"share", SDL_GAMEPAD_BUTTON_BACK},
      {"guide", SDL_GAMEPAD_BUTTON_GUIDE},    {"ps", SDL_GAMEPAD_BUTTON_GUIDE},
      {"start", SDL_GAMEPAD_BUTTON_START},    {"options", SDL_GAMEPAD_BUTTON_START},
      {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK},  {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
      {"l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
      {"r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
      {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},     {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
      {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
      {"touch", SDL_GAMEPAD_BUTTON_TOUCHPAD},
      {"misc", SDL_GAMEPAD_BUTTON_MISC1},
  }};
  for (const auto& entry : NAMES)
  {
    if (entry.name == n)
      return 1u << entry.button;
  }
  return 0;
}

void LoadScript(const char* path)
{
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
  {
    line = line.substr(0, line.find('#'));
    std::istringstream words(line);
    std::string first, names;
    if (!(words >> first))
      continue;
    if (first == "loop")
    {
      words >> s_loop_start >> s_loop_len;
      continue;
    }
    Step step;
    char* end = nullptr;
    step.until = static_cast<u32>(std::strtoul(first.c_str(), &end, 0));
    if (end == first.c_str() || *end != '\0')
    {
      Note(fmt::format("script: bad line '{}'", line));
      continue;
    }
    words >> names;
    std::transform(names.begin(), names.end(), names.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (names != "none")
    {
      std::istringstream parts(names);
      std::string part;
      while (std::getline(parts, part, '+'))
      {
        if (part == "l2")
          step.l2 = true;
        else if (part == "r2")
          step.r2 = true;
        else if (const u32 bit = ButtonBit(part))
          step.buttons |= bit;
        else
          Note(fmt::format("script: unknown button '{}'", part));
      }
    }
    for (int& v : step.sticks)
    {
      if (!(words >> v))
        break;
      v = std::clamp(v, -127, 127);
    }
    s_script.push_back(step);
  }
  Note(fmt::format("script {} steps, loop {}+{}", s_script.size(), s_loop_start, s_loop_len));
}

bool Rumble(void*, Uint16, Uint16)
{
  return true;
}
bool SetLED(void*, Uint8, Uint8, Uint8)
{
  return true;
}

void Attach(int order, const Kind& kind)
{
  // The real pads' layout as far as an SDL virtual gamepad allows: the 11 face/
  // shoulder/stick/menu buttons, the D-pad (buttons here; a hat on HIDAPI), the
  // DualShock/DualSense touchpad click, six axes (sticks + analog triggers).
  u32 mask = (1u << 11) - 1;
  mask |= (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) | (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
          (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) | (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
  if (kind.touchpad)
    mask |= 1u << SDL_GAMEPAD_BUTTON_TOUCHPAD;
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.vendor_id = kind.vendor;
  desc.product_id = kind.product;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = static_cast<Uint16>(std::popcount(mask));
  desc.button_mask = mask;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  desc.name = kind.name;
  desc.Rumble = Rumble;
  desc.SetLED = SetLED;
  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  if (id == 0)
  {
    Note(fmt::format("attach {} failed: {}", kind.key, SDL_GetError()));
    return;
  }
  SDL_Joystick* js = SDL_OpenJoystick(id);
  {
    std::lock_guard lk(s_mutex);
    s_pads.push_back({&kind, id, js, mask});
    s_order.push_back(order);
  }
  Note(fmt::format("attached #{} {} '{}' {:04x}:{:04x} sdl_instance={} gamepad={}", order,
                   kind.key, kind.name, kind.vendor, kind.product, id, SDL_IsGamepad(id)));
}

// Field count since boot on the emulated clock (0 before the core runs).
u32 Field()
{
  auto& system = Core::System::GetInstance();
  const u32 per_field = system.GetVideoInterface().GetTicksPerField();
  if (per_field == 0)
    return 0;
  return static_cast<u32>(system.GetCoreTiming().GetTicks() / per_field);
}

void Apply(const Pad& pad, const Step& step)
{
  int index = 0;  // virtual button index = rank of the SDL button in the mask
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
  {
    if (!(pad.button_mask & (1u << b)))
      continue;
    SDL_SetJoystickVirtualButton(pad.joystick, index++, ((step.buttons >> b) & 1) != 0);
  }
  const auto axis = [](int v) { return static_cast<Sint16>(v * 32767 / 127); };
  SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX, axis(step.sticks[0]));
  SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTY, axis(-step.sticks[1]));
  SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis(step.sticks[2]));
  SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis(-step.sticks[3]));
  SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                             step.l2 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
  SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                             step.r2 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
}
}  // namespace

void AttachFromEnv()
{
  const char* list = std::getenv("NSR_TEST_VIRTUAL_PADS");
  if (!list || !*list)
    return;
  if (const char* log = std::getenv("NSR_TEST_PAD_LOG"))
    s_log = std::fopen(log, "a");
  if (const char* pad = std::getenv("NSR_TEST_PAD_SCRIPT_PAD"))
    s_script_pad = std::atoi(pad);
  if (const char* script = std::getenv("NSR_TEST_PAD_SCRIPT"))
    LoadScript(script);
  std::istringstream items(list);
  std::string item;
  int order = 0;
  while (std::getline(items, item, ','))
  {
    const auto at = item.find('@');
    const std::string key = item.substr(0, at);
    const int delay_ms = at == std::string::npos ? 0 : std::atoi(item.c_str() + at + 1);
    const auto kind = std::find_if(KINDS.begin(), KINDS.end(),
                                   [&](const Kind& k) { return key == k.key; });
    if (kind == KINDS.end())
    {
      Note(fmt::format("unknown pad kind '{}'", key));
      ++order;
      continue;
    }
    if (delay_ms <= 0)
    {
      Attach(order, *kind);
    }
    else
    {
      Note(fmt::format("#{} {} attaches in {} ms", order, kind->key, delay_ms));
      s_late.emplace_back([order, kind, delay_ms] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        while (!s_stop && std::chrono::steady_clock::now() < until)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!s_stop)
          Attach(order, *kind);
      });
    }
    ++order;
  }
}

void Drive()
{
  std::lock_guard lk(s_mutex);
  if (s_script.empty())
    return;
  const auto it = std::find(s_order.begin(), s_order.end(), s_script_pad);
  if (it == s_order.end())
    return;  // not plugged in yet
  const Pad& pad = s_pads[it - s_order.begin()];
  u32 field = Field();
  const u32 raw_field = field;
  if (s_loop_len != 0 && field >= s_loop_start)
    field = s_loop_start + (field - s_loop_start) % s_loop_len;
  Step step{};  // past the end: released
  for (const Step& s : s_script)
  {
    if (field < s.until)
    {
      step = s;
      break;
    }
  }
  if (s_have_applied && step == s_applied)
    return;
  Apply(pad, step);
  s_applied = step;
  s_have_applied = true;
  Note(fmt::format("field {} pad #{} buttons {:#07x} l2 {} r2 {} sticks {} {} {} {}", raw_field,
                   s_script_pad, step.buttons, step.l2, step.r2, step.sticks[0], step.sticks[1],
                   step.sticks[2], step.sticks[3]));
}

void Shutdown()
{
  s_stop = true;
  for (auto& t : s_late)
    t.join();
  s_late.clear();
  std::lock_guard lk(s_mutex);
  for (const Pad& pad : s_pads)
  {
    if (pad.joystick)
      SDL_CloseJoystick(pad.joystick);
    SDL_DetachVirtualJoystick(pad.id);
  }
  s_pads.clear();
  s_order.clear();
  if (s_log)
  {
    std::fclose(s_log);
    s_log = nullptr;
  }
}
}  // namespace ciface::SDL::NSRTestVirtualPads
