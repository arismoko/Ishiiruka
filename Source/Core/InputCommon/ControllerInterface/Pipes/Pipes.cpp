// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <array>
#include <cstdlib>
#include <iostream>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include "Common/Logging/Log.h"
#include "Common/FileUtil.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Pipes/Pipes.h"
#include "Core/ConfigManager.h"

namespace ciface
{
namespace Pipes
{
static const std::array<std::string, 12> s_button_tokens{
    {"A", "B", "X", "Y", "Z", "START", "L", "R", "D_UP", "D_DOWN", "D_LEFT", "D_RIGHT"}};

static const std::array<std::string, 2> s_shoulder_tokens{{"L", "R"}};

static const std::array<std::string, 2> s_axis_tokens{{"MAIN", "C"}};

static double StringToDouble(const std::string& text)
{
  std::istringstream is(text);
  // ignore current locale
  is.imbue(std::locale::classic());
  double result;
  is >> result;
  return result;
}

void PopulateDevices()
{
  #ifdef _WIN32
  PIPE_FD pipes[4];
  // Windows has named pipes, but they're different. They don't exist on the
  //  local filesystem and are transient. So rather than searching the /Pipes
  //  directory for pipes, we just always assume there's 4 and then make them
  for (uint32_t i = 0; i < 4; i++)
  {
    std::string pipename = "\\\\.\\pipe\\slippibot" + std::to_string(i+1);
    pipes[i] = CreateNamedPipeA(
       pipename.data(),              // pipe name
       PIPE_ACCESS_INBOUND,          // read access, inward only
       PIPE_TYPE_BYTE | PIPE_NOWAIT, // byte mode, nonblocking
       1,                            // number of clients
       256,                          // output buffer size
       256,                          // input buffer size
       0,                            // timeout value
       NULL                          // security attributes
    );

    // We're in nonblocking mode, so this won't wait for clients
    ConnectNamedPipe(pipes[i], NULL);
    std::string ui_pipe_name = "slippibot" + std::to_string(i+1);
    g_controller_interface.AddDevice(std::make_shared<PipeDevice>(pipes[i], ui_pipe_name));
  }
  #else

  // Search the Pipes directory for files that we can open in read-only,
  // non-blocking mode. The device name is the virtual name of the file.
  File::FSTEntry fst;
  std::string dir_path = File::GetUserPath(D_PIPES_IDX);
  if (!File::Exists(dir_path))
    return;
  fst = File::ScanDirectoryTree(dir_path, false);
  if (!fst.isDirectory)
    return;
  for (unsigned int i = 0; i < fst.size; ++i)
  {
    const File::FSTEntry& child = fst.children[i];
    if (child.isDirectory)
      continue;
    PIPE_FD fd = open(child.physicalName.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    g_controller_interface.AddDevice(
        std::make_shared<PipeDevice>(fd, child.virtualName, child.physicalName));
  }
  #endif
}

PipeDevice::PipeDevice(PIPE_FD fd, const std::string& name, const std::string& pipepath)
    : m_fd(fd), m_name(name), m_pipepath(pipepath)
{
#ifndef _WIN32
  // Warn if no path was supplied on POSIX: the device will work but will not
  // be able to reopen the FIFO after all writers disconnect.
  if (m_pipepath.empty())
    WARN_LOG(SLIPPI, "PipeDevice '%s': no filesystem path supplied; FIFO reconnection disabled",
             m_name.c_str());
#endif
  for (const auto& tok : s_button_tokens)
  {
    PipeInput* btn = new PipeInput("Button " + tok);
    AddInput(btn);
    m_buttons[tok] = btn;
  }
  for (const auto& tok : s_shoulder_tokens)
  {
    AddAxis(tok, 0.0);
  }
  for (const auto& tok : s_axis_tokens)
  {
    AddAxis(tok + " X", 0.5);
    AddAxis(tok + " Y", 0.5);
  }
}

PipeDevice::~PipeDevice()
{
  #ifdef _WIN32
  CloseHandle(m_fd);
  #else
  if (m_fd >= 0)
    close(m_fd);
  #endif
}

s32 PipeDevice::readFromPipe(PIPE_FD file_descriptor, char *in_buffer, size_t size)
{
  #ifdef _WIN32

  u32 bytes_available = 0;
  DWORD bytesread = 0;
  bool peek_success = PeekNamedPipe(
    file_descriptor,
    NULL,
    0,
    NULL,
    (LPDWORD)&bytes_available,
    NULL
  );

  if(!peek_success && (GetLastError() == ERROR_BROKEN_PIPE))
  {
    DisconnectNamedPipe(file_descriptor);
    ConnectNamedPipe(file_descriptor, NULL);
    return -1;
  }

  if(peek_success && (bytes_available > 0))
  {
    bool success = ReadFile(
      file_descriptor,    // pipe handle
      in_buffer,          // buffer to receive reply
      (DWORD)std::min(bytes_available, (u32)size),        // size of buffer
      &bytesread,         // number of bytes read
      NULL);              // not overlapped
    if(!success)
    {
        return -1;
    }
  }
  return (s32)bytesread;
  #else
  return read(file_descriptor, in_buffer, size);
  #endif
}

void PipeDevice::UpdateInput()
{
  bool finished = false;

#ifndef _WIN32
  // Guard against an invalid fd left by a failed reopen attempt.
  if (m_fd < 0)
  {
    // Only log once to avoid spamming the log every frame.
    static bool s_logged = false;
    if (!s_logged)
    {
      WARN_LOG(SLIPPI, "PipeDevice '%s': fd is invalid (last reopen failed); skipping updates until FIFO becomes available",
               m_name.c_str());
      s_logged = true;
    }
    return;
  }
#endif

  // In blocking-pipes mode we must not return until we have received a FLUSH
  // command (which signals end-of-frame) or until the pipe becomes
  // unrecoverable.  The two platforms need different wait primitives.
#ifdef _WIN32
  if (SConfig::GetInstance().m_blockingPipes && g_needInputForFrame)
  {
    // Poll with a 1 ms sleep until at least one byte is available.
    // This mirrors the POSIX select() block below and keeps CPU usage low.
    while (g_needInputForFrame)
    {
      DWORD bytes_available = 0;
      BOOL peek_ok = PeekNamedPipe(m_fd, NULL, 0, NULL, &bytes_available, NULL);
      if (!peek_ok)
      {
        // Client disconnected; reset the pipe so a new client can connect.
        DisconnectNamedPipe(m_fd);
        ConnectNamedPipe(m_fd, NULL);
        return;
      }
      if (bytes_available > 0)
        break;
      Sleep(1);
    }
  }
#else
  if (SConfig::GetInstance().m_blockingPipes && g_needInputForFrame)
  {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(m_fd, &set);

    // Block until data arrives on the FIFO.
    select(m_fd + 1, &set, NULL, NULL, NULL);
  }
#endif

  do
  {
    // Read any pending bytes off the pipe.  When we accumulate a newline,
    // dequeue the command at the front of m_buf and parse it.
    char buf[32];
    s32 bytes_read = readFromPipe(m_fd, buf, sizeof buf);

#ifdef _WIN32
    // On Windows, readFromPipe returns -1 on a broken-pipe error (and
    // already called Disconnect/ConnectNamedPipe internally).  0 means
    // there is simply no data yet, which is fine in non-blocking mode.
    if (bytes_read < 0)
      return;
#else
    // On POSIX, read() returns 0 only when all writers have closed the
    // FIFO (EOF).  Attempt to reopen it so that a restarted bridge can
    // reconnect without restarting Dolphin.
    if (bytes_read == 0)
    {
      if (!m_pipepath.empty())
      {
        close(m_fd);
        m_fd = open(m_pipepath.c_str(), O_RDONLY | O_NONBLOCK);
        if (m_fd < 0)
          ERROR_LOG(SLIPPI,
                    "PipeDevice '%s': failed to reopen FIFO '%s' after EOF (errno %d)",
                    m_name.c_str(), m_pipepath.c_str(), errno);
      }
      return;
    }
#endif

    while (bytes_read > 0)
    {
      m_buf.append(buf, bytes_read);
      bytes_read = readFromPipe(m_fd, buf, sizeof buf);
    }
    std::size_t newline = m_buf.find("\n");
    while (newline != std::string::npos)
    {
      std::string command = m_buf.substr(0, newline);
      finished = ParseCommand(command);

      m_buf.erase(0, newline + 1);
      newline = m_buf.find("\n");
    }
  } while (!finished && g_needInputForFrame && SConfig::GetInstance().m_blockingPipes);
}

void PipeDevice::AddAxis(const std::string& name, double value)
{
  // Dolphin uses separate axes for left/right, which complicates things.
  PipeInput* ax_hi = new PipeInput("Axis " + name + " +");
  ax_hi->SetState(value);
  PipeInput* ax_lo = new PipeInput("Axis " + name + " -");
  ax_lo->SetState(value);
  m_axes[name + " +"] = ax_hi;
  m_axes[name + " -"] = ax_lo;
  AddAnalogInputs(ax_lo, ax_hi);
}

void PipeDevice::SetAxis(const std::string& entry, double value)
{
  value = MathUtil::Clamp(value, 0.0, 1.0);
  double hi = std::max(0.0, value - 0.5) * 2.0;
  double lo = (0.5 - std::min(0.5, value)) * 2.0;
  auto search_hi = m_axes.find(entry + " +");
  if (search_hi != m_axes.end())
    search_hi->second->SetState(hi);
  auto search_lo = m_axes.find(entry + " -");
  if (search_lo != m_axes.end())
    search_lo->second->SetState(lo);
}

bool PipeDevice::ParseCommand(const std::string& command)
{
  if(command == "FLUSH")
  {
    g_needInputForFrame = false;
    return true;
  }
  std::vector<std::string> tokens;
  SplitString(command, ' ', tokens);
  if (tokens.size() < 2 || tokens.size() > 4)
    return false;
  if (tokens[0] == "PRESS" || tokens[0] == "RELEASE")
  {
    auto search = m_buttons.find(tokens[1]);
    if (search != m_buttons.end())
      search->second->SetState(tokens[0] == "PRESS" ? 1.0 : 0.0);
  }
  else if (tokens[0] == "SET")
  {
    if (tokens.size() == 3)
    {
      double value = StringToDouble(tokens[2]);
      SetAxis(tokens[1], (value / 2.0) + 0.5);
    }
    else if (tokens.size() == 4)
    {
      double x = StringToDouble(tokens[2]);
      double y = StringToDouble(tokens[3]);
      SetAxis(tokens[1] + " X", x);
      SetAxis(tokens[1] + " Y", y);
    }
  }
  return false;
}
}
}
