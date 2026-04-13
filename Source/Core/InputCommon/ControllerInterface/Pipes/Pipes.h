// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

extern bool g_needInputForFrame;

namespace ciface
{
namespace Pipes
{
  #ifdef _WIN32
  typedef HANDLE PIPE_FD;
  #else
  typedef int PIPE_FD;
  #endif

// Pipe-input controller backend.  External processes write newline-separated
// commands to a named pipe and Dolphin maps them onto a virtual GC controller.
//
// Platform setup
// ==============
// Windows: Dolphin creates four server-side named pipes automatically:
//     \\.\pipe\slippibot1  through  \\.\pipe\slippibot4
//   Clients open these as ordinary Win32 file handles and write to them.
//   The device appears in the input UI as "Pipe/0/slippibot1" etc.
//
// Linux/macOS: Create a FIFO (named pipe) in the user Pipes directory, e.g.:
//     mkfifo ~/.config/dolphin-emu/Pipes/slippibot1
//   Dolphin opens every regular file/FIFO in that directory at startup.
//   The device name is the FIFO's filename ("slippibot1"), so the same
//   slippibot.ini profile works on both platforms without modification.
//   If all writers close the FIFO, Dolphin will reopen it automatically
//   so that a restarted bridge reconnects without restarting Dolphin.
//
// Command syntax (newline-delimited, space-separated tokens, case-sensitive)
// ==========================================================================
// PRESS   {A|B|X|Y|Z|START|L|R|D_UP|D_DOWN|D_LEFT|D_RIGHT}
// RELEASE {A|B|X|Y|Z|START|L|R|D_UP|D_DOWN|D_LEFT|D_RIGHT}
// SET {L|R}        <value>          -- shoulder analog [0, 1]
// SET {MAIN|C}     <x-value> <y-value>  -- stick axes   [0, 1] each
// FLUSH  -- in blocking-pipes mode, signals that all inputs for this frame
//           have been delivered and the emulator may advance one frame.
//           Has no effect in non-blocking mode.
void PopulateDevices();

class PipeDevice : public Core::Device
{
public:
  // pipepath is the physical filesystem path used to reopen the FIFO on EOF
  // (Linux/macOS only; ignored on Windows where the same HANDLE is reused).
  PipeDevice(PIPE_FD fd, const std::string& name, const std::string& pipepath = "");
  ~PipeDevice();

  void UpdateInput() override;
  std::string GetName() const override { return m_name; }
  std::string GetSource() const override { return "Pipe"; }
private:
  class PipeInput : public Input
  {
  public:
    PipeInput(const std::string& name) : m_name(name), m_state(0.0) {}
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override { return m_state; }
    void SetState(ControlState state) { m_state = state; }
  private:
    const std::string m_name;
    ControlState m_state;
  };

  void AddAxis(const std::string& name, double value);
  bool ParseCommand(const std::string& command);
  void SetAxis(const std::string& entry, double value);
  s32 readFromPipe(PIPE_FD file_descriptor, char *in_buffer, size_t size);

  // m_fd is mutable on POSIX so we can reopen a closed FIFO in UpdateInput.
  PIPE_FD m_fd;
  const std::string m_name;
  // Physical filesystem path of the FIFO (POSIX only); empty on Windows.
  const std::string m_pipepath;
  std::string m_buf;
  std::map<std::string, PipeInput*> m_buttons;
  std::map<std::string, PipeInput*> m_axes;
};
}
}
