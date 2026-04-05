// win_input.h — Native Windows SendInput for typing and special commands
// Zero dependencies beyond user32.dll (loaded by Windows automatically)
#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace WinInput {

// ─── SendInput helpers ─────────────────────────────────────────────────────

static void pressKey(WORD vk, bool extended = false) {
  INPUT inputs[2] = {};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = vk;
  inputs[0].ki.dwFlags = extended ? KEYEVENTF_EXTENDEDKEY : 0;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = vk;
  inputs[1].ki.dwFlags =
      KEYEVENTF_KEYUP | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
  SendInput(2, inputs, sizeof(INPUT));
}

static void pressCombo(WORD mod, WORD key) {
  INPUT inputs[4] = {};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = mod;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = key;
  inputs[2].type = INPUT_KEYBOARD;
  inputs[2].ki.wVk = key;
  inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
  inputs[3].type = INPUT_KEYBOARD;
  inputs[3].ki.wVk = mod;
  inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(4, inputs, sizeof(INPUT));
}

static void typeUnicode(const std::wstring &text) {
  // Type each character via KEYEVENTF_UNICODE — works in any window/layout
  std::vector<INPUT> inputs;
  inputs.reserve(text.size() * 2);
  for (wchar_t ch : text) {
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = ch;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    inputs.push_back(down);

    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    inputs.push_back(up);
  }
  if (!inputs.empty())
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

static void typeUtf8(const std::string &utf8) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (wlen <= 1)
    return;
  std::wstring wide(wlen - 1, 0);
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wlen);
  typeUnicode(wide);
}

// ─── Normalize for command matching ────────────────────────────────────────

static std::string normalize(const std::string &text) {
  std::string s = text;
  // Lowercase
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  // Strip non-alphanumeric (keep spaces)
  std::string clean;
  for (char c : s) {
    if (std::isalnum((unsigned char)c) || c == ' ')
      clean += c;
  }
  // Collapse whitespace
  std::string result;
  bool lastSpace = false;
  for (char c : clean) {
    if (c == ' ') {
      if (!lastSpace && !result.empty()) {
        result += ' ';
        lastSpace = true;
      }
    } else {
      result += c;
      lastSpace = false;
    }
  }
  // Trim trailing space
  while (!result.empty() && result.back() == ' ')
    result.pop_back();
  return result;
}

// ─── Special command table ────────────────────────────────────────────────

using CmdFn = std::function<void()>;

static const std::unordered_map<std::string, CmdFn> &getCommands() {
  static const std::unordered_map<std::string, CmdFn> cmds = {
      {"space", [] { pressKey(VK_SPACE); }},
      {"spacebar", [] { pressKey(VK_SPACE); }},
      {"space bar", [] { pressKey(VK_SPACE); }},
      {"backspace", [] { pressKey(VK_BACK); }},
      {"back space", [] { pressKey(VK_BACK); }},
      {"enter", [] { pressKey(VK_RETURN); }},
      {"return", [] { pressKey(VK_RETURN); }},
      {"new line", [] { pressKey(VK_RETURN); }},
      {"newline", [] { pressKey(VK_RETURN); }},
      {"tab", [] { pressKey(VK_TAB); }},
      {"delete", [] { pressKey(VK_DELETE, true); }},
      {"escape", [] { pressKey(VK_ESCAPE); }},
      {"esc", [] { pressKey(VK_ESCAPE); }},
      {"select all", [] { pressCombo(VK_CONTROL, 'A'); }},
      {"undo", [] { pressCombo(VK_CONTROL, 'Z'); }},
      {"redo", [] { pressCombo(VK_CONTROL, 'Y'); }},
      {"copy", [] { pressCombo(VK_CONTROL, 'C'); }},
      {"paste", [] { pressCombo(VK_CONTROL, 'V'); }},
      {"cut", [] { pressCombo(VK_CONTROL, 'X'); }},
      {"save", [] { pressCombo(VK_CONTROL, 'S'); }},
      {"find", [] { pressCombo(VK_CONTROL, 'F'); }},
      {"left", [] { pressKey(VK_LEFT, true); }},
      {"right", [] { pressKey(VK_RIGHT, true); }},
      {"up", [] { pressKey(VK_UP, true); }},
      {"down", [] { pressKey(VK_DOWN, true); }},
      {"home", [] { pressKey(VK_HOME, true); }},
      {"end", [] { pressKey(VK_END, true); }},
      {"windows", [] { pressKey(VK_LWIN); }},
      {"win", [] { pressKey(VK_LWIN); }},
      {"caps lock", [] { pressKey(VK_CAPITAL); }},
      {"caps", [] { pressKey(VK_CAPITAL); }},
  };
  return cmds;
}

// Returns (true, cmd_name) if executed, (false, "") if not a command
static std::pair<bool, std::string> tryCommand(const std::string &text) {
  std::string norm = normalize(text);
  if (norm.empty())
    return {false, ""};
  auto &cmds = getCommands();
  auto it = cmds.find(norm);
  if (it != cmds.end()) {
    it->second();
    return {true, norm};
  }
  return {false, ""};
}

// Handle transcription: special command or type text
static void handleTranscription(const std::string &text) {
  if (text.empty())
    return;
  auto [wasCmd, cmdName] = tryCommand(text);
  if (wasCmd)
    return;
  typeUtf8(text + " ");
}

} // namespace WinInput
