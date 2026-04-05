#Requires AutoHotkey v2.0
#SingleInstance Force
#NoTrayIcon

SendMode "Input"
SetKeyDelay -1, -1

stdin := FileOpen("*", "r", "UTF-8")
stdout := FileOpen("*", "w", "UTF-8")
stdout.NewLine := "`n"

WriteResp(id, kind, statusText) {
  global stdout
  stdout.Write("RESP`t" id "`t" kind "`t" statusText "`n")
  stdout.Flush()
}

NormalizeForCommand(text) {
  text := StrLower(Trim(text))
  text := RegExReplace(text, "[^\\p{L}\\p{N} ]+", "")
  text := RegExReplace(text, "\\s+", " ")
  return text
}

IsTextInputFocused() {
  focus := ""
  try focus := ControlGetFocus("A")
  ; focus is usually ClassNN (ex: Edit1, RichEdit20W2, Chrome_RenderWidgetHostHWND1)
  if (focus != "") {
    if RegExMatch(focus, "i)^(Edit|RichEdit|RICHEDIT|Scintilla|TextBox|TMemo|TEdit|WindowsForms10\\.EDIT)")
      return true
    if RegExMatch(focus, "i)(Edit|RichEdit|Scintilla|TextBox|TMemo|TEdit)")
      return true
  }

  x := 0, y := 0
  try {
    if (CaretGetPos(&x, &y))
      return true
  }
  return false
}

Loop {
  line := ""
  try line := stdin.ReadLine()
  catch {
    break
  }

  if (line = "")
    continue
  line := RTrim(line, "`r`n")

  if !RegExMatch(line, "^(\\d+)\\t([01])\\t(.*)$", &m)
    continue

  id := m[1]
  enabled := m[2]
  text := Trim(m[3])
  if (text = "") {
    WriteResp(id, "noop", "Idling...")
    continue
  }

  norm := NormalizeForCommand(text)
  if (enabled = "1") {
    if (norm = "space" || norm = "spacebar" || norm = "space bar") {
      Send "{Space}"
      WriteResp(id, "cmd", "Command: Space")
      continue
    }
    if (norm = "back space" || norm = "backspace") {
      Send "{Backspace}"
      WriteResp(id, "cmd", "Command: Backspace")
      continue
    }
    if (norm = "control" || norm = "ctrl") {
      Send "{Ctrl down}{Ctrl up}"
      WriteResp(id, "cmd", "Command: Control")
      continue
    }
    if (norm = "escape" || norm = "esc") {
      if (!IsTextInputFocused()) {
        Send "{Esc}"
        WriteResp(id, "cmd", "Command: Escape")
        continue
      }
    }
    if (norm = "windows" || norm = "win") {
      if (!IsTextInputFocused()) {
        Send "{LWin down}{LWin up}"
        WriteResp(id, "cmd", "Command: Windows")
        continue
      }
    }
    if (norm = "tab") {
      if (!IsTextInputFocused()) {
        Send "{Tab}"
        WriteResp(id, "cmd", "Command: Tab")
        continue
      }
    }
  }

  SendText text
  WriteResp(id, "type", "Typing: " text)
}


