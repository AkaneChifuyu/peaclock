#include "peaclock/tui.hh"

#include "ob/num.hh"
#include "ob/algorithm.hh"
#include "ob/string.hh"
#include "ob/text.hh"
#include "ob/term.hh"
namespace aec = OB::Term::ANSI_Escape_Codes;

#include <ctime>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#include <regex>
#include <utility>
#include <optional>
#include <limits>

#include <filesystem>
namespace fs = std::filesystem;

using std::string_literals::operator""s;

// bool to string
#define btos(x) ("off\0on"+4*!!(x))

Tui::Tui() :
  _colorterm {OB::Term::is_colorterm()}
{
  _ctx.prompt.timeout = _ctx.prompt.rate.get() / _ctx.refresh_rate.get();
}

bool Tui::press_to_continue(std::string const& str, char32_t val)
{
  std::cerr
  << "Press " << str << " to continue";

  _term_mode.set_min(1);
  _term_mode.set_raw();

  bool res {false};
  char32_t key {0};
  if ((key = OB::Term::get_key()) > 0)
  {
    res = (val == 0 ? true : val == key);
  }

  _term_mode.set_cooked();

  std::cerr
  << aec::nl;

  return res;
}

void Tui::base_config(fs::path const& path)
{
  _ctx.base_config = path;
}

void Tui::load_config(fs::path const& path)
{
  // ignore config if path equals "NONE"
  if (path == "NONE")
  {
    return;
  }

  // buffer for error output
  std::ostringstream err;

  if (! path.empty() && fs::exists(path))
  {
    std::ifstream file {path};

    if (file.is_open())
    {
      std::string line;
      std::size_t lnum {0};

      while (std::getline(file, line))
      {
        // increase line number
        ++lnum;

        // trim leading and trailing whitespace
        line = OB::String::trim(line);

        // ignore empty line or comment
        if (line.empty() || OB::String::assert_rx(line, std::regex("^#[^\\r]*$")))
        {
          continue;
        }

        if (auto const res = command(line))
        {
          if (! res.value().first)
          {
            // source:line: level: info
            err << path.string() << ":" << lnum << ": " << res.value().second << "\n";
          }
        }
      }
    }
    else
    {
      err << "error: could not open config file '" << path.string() << "'\n";
    }
  }
  else
  {
    err << "error: the file '" << path.string() << "' does not exist\n";
  }

  if (! err.str().empty())
  {
    std::cerr << err.str();

    if (! press_to_continue("ENTER", '\n'))
    {
      throw std::runtime_error("aborted by user");
    }
  }
}

void Tui::load_hist_command(fs::path const& path)
{
  _readline.hist_load(path);
}

// bool Tui::mkconfig()
// {
//   if (_ctx.base_config.empty())
//   {
//     set_status(false, "empty config directory");

//     return false;
//   }

//   fs::path path {_ctx.base_config / fs::path("state") / fs::path(_fltrdr.content_id())};

//   std::ofstream file {path, std::ios::trunc};

//   if (! file.is_open())
//   {
//     set_status(false, "could not open file");

//     return false;
//   }

//   // timestamp
//   std::time_t t = std::time(0);
//   std::tm tm = *std::localtime(&t);

//   // dump current state to file
//   file
//   << "# peaclock config\n"
//   << "# file: " << _ctx.file.path.string() << "\n"
//   << "# date: " << std::put_time(&tm, "%FT%TZ\n") << "\n\n"
//   << std::flush;

//   set_status(true, "");

//   return true;
// }

void Tui::run()
{
  std::cout
  << aec::cursor_hide
  << aec::screen_push
  << aec::cursor_hide
  << aec::screen_clear
  << aec::cursor_home
  << aec::mouse_enable
  << std::flush;

  // set terminal mode to raw
  _term_mode.set_min(0);
  _term_mode.set_raw();

  // start the event loop
  event_loop();

  std::cout
  << aec::mouse_disable
  << aec::nl
  << aec::screen_pop
  << aec::cursor_show
  << std::flush;
}

void Tui::event_loop()
{
  while (_ctx.is_running)
  {
    // get the terminal width and height
    OB::Term::size(_ctx.width, _ctx.height);

    // check for correct screen size
    if (screen_size() != 0)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(_ctx.input_interval));

      char32_t key {0};
      if ((key = OB::Term::get_key()) > 0)
      {
        switch (key)
        {
          case 'q': case 'Q':
          case OB::Term::ctrl_key('c'):
          {
            _ctx.is_running = false;

            break;
          }

          default:
          {
            break;
          }
        }
      }

      continue;
    }

    // render new content
    clear();
    draw();
    refresh();

    int wait {_ctx.refresh_rate.get()};
    int tick {0};

    while (_ctx.is_running && wait)
    {
      if (wait > _ctx.input_interval.get())
      {
        tick = _ctx.input_interval.get();
        wait -= _ctx.input_interval.get();
      }
      else
      {
        tick = wait;
        wait = 0;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(tick));

      get_input();
    }
  }
}

void Tui::clear()
{
  // clear screen
  _ctx.buf
  << aec::cursor_home
  << _ctx.style.background;

  OB::Algorithm::for_each(_ctx.height,
  [&](auto) {
    OB::Algorithm::for_each(_ctx.width, [&](auto) {
      _ctx.buf << " "; });
    _ctx.buf << "\n";
  },
  [&](auto) {
    OB::Algorithm::for_each(_ctx.width, [&](auto) {
      _ctx.buf << " "; });
  });

  _ctx.buf
  << aec::clear
  << aec::cursor_home;
}

void Tui::refresh()
{
  // output buffer to screen
  std::cout
  << _ctx.buf.str()
  << std::flush;

  // clear output buffer
  _ctx.buf.str("");
}

void Tui::draw()
{
  draw_content();
  draw_prompt_message();
  draw_keybuf();
}

void Tui::draw_content()
{
  _ctx.buf
  << aec::cursor_save;

  // render new content
  _peaclock.render(_ctx.width, _ctx.height, _ctx.buf);

  _ctx.buf
  << aec::clear
  << aec::cursor_load;
}

void Tui::draw_keybuf()
{
  if (_ctx.keys.empty())
  {
    return;
  }

  _ctx.buf
  << aec::cursor_save
  << aec::cursor_set(_ctx.width - 3, _ctx.height)
  << _ctx.style.background
  << "    "
  << aec::cursor_set(_ctx.width - 3, _ctx.height)
  << _ctx.style.text
  << aec::space;

  for (auto const& e : _ctx.keys)
  {
    if (OB::Text::is_print(static_cast<std::int32_t>(e.val)))
    {
      _ctx.buf
      << e.str;
    }
  }

  _ctx.buf
  << aec::space
  << aec::clear
  << aec::cursor_load;
}

void Tui::draw_prompt_message()
{
  // check if command prompt message is active
  if (_ctx.prompt.count > 0)
  {
    --_ctx.prompt.count;

    _ctx.buf
    << aec::cursor_save
    << aec::cursor_set(0, _ctx.height)
    << _ctx.style.background
    << _ctx.style.prompt
    << ">"
    << _ctx.style.prompt_status
    << _ctx.prompt.str.substr(0, _ctx.width - 5)
    << aec::cursor_load;
  }
}

void Tui::set_status(bool success, std::string const& msg)
{
  _ctx.style.prompt_status = success ? _ctx.style.success : _ctx.style.error;
  _ctx.prompt.str = msg;
  _ctx.prompt.count = _ctx.prompt.timeout;
}

void Tui::get_input()
{
  if ((_ctx.key.val = OB::Term::get_key(&_ctx.key.str)) > 0)
  {
    _ctx.keys.emplace_back(_ctx.key);

    switch (_ctx.keys.at(0).val)
    {
      // quit
      case 'q': case 'Q':
      {
        _ctx.is_running = false;
        _ctx.keys.clear();

        return;
      }

      case OB::Term::ctrl_key('c'):
      {
        _ctx.is_running = false;
        _ctx.keys.clear();

        return;
      }

      case OB::Term::Key::escape:
      {
        _ctx.prompt.count = 0;
        _ctx.keys.clear();

        break;
      }

      // command prompt
      case ':':
      {
        command_prompt();
        _ctx.keys.clear();

        break;
      }

      case 'a':
      {
        _peaclock.cfg.hour_24 = ! _peaclock.cfg.hour_24;
        set_status(true, "set hour-24 "s + btos(_peaclock.cfg.hour_24));

        break;
      }

      case 's':
      {
        _peaclock.cfg.seconds = ! _peaclock.cfg.seconds;
        set_status(true, "set seconds "s + btos(_peaclock.cfg.seconds));

        break;
      }

      case 'd':
      {
        _peaclock.cfg.date = ! _peaclock.cfg.date;
        set_status(true, "set date "s + btos(_peaclock.cfg.date));

        break;
      }

      case 'f':
      {
        _peaclock.cfg.auto_size = ! _peaclock.cfg.auto_size;
        set_status(true, "set auto-size "s + btos(_peaclock.cfg.auto_size));

        break;
      }

      case 'g':
      {
        _peaclock.cfg.auto_ratio = ! _peaclock.cfg.auto_ratio;
        set_status(true, "set auto-ratio "s + btos(_peaclock.cfg.auto_ratio));

        break;
      }

      case 'h':
      {
        switch (_peaclock.cfg.toggle)
        {
          case Peaclock::Toggle::block:
          {
            --_peaclock.cfg.x_block;
            set_status(true, "block-x " + _peaclock.cfg.x_block.str());

            break;
          }

          case Peaclock::Toggle::padding:
          {
            --_peaclock.cfg.x_space;
            set_status(true, "padding-x " + _peaclock.cfg.x_space.str());

            break;
          }

          case Peaclock::Toggle::margin:
          {
            --_peaclock.cfg.x_border;
            set_status(true, "margin-x " + _peaclock.cfg.x_border.str());

            break;
          }

          case Peaclock::Toggle::ratio:
          {
            --_peaclock.cfg.x_ratio;
            set_status(true, "ratio-x " + _peaclock.cfg.x_ratio.str());

            break;
          }

          case Peaclock::Toggle::active_fg:
          {
            _peaclock.cfg.style.active_fg.hue(_peaclock.cfg.style.active_fg.hue() - 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.active_fg.hue()));

            break;
          }

          case Peaclock::Toggle::active_bg:
          {
            _peaclock.cfg.style.active_bg.hue(_peaclock.cfg.style.active_bg.hue() - 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.active_bg.hue()));

            break;
          }

          case Peaclock::Toggle::inactive_fg:
          {
            _peaclock.cfg.style.inactive_fg.hue(_peaclock.cfg.style.inactive_fg.hue() - 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.inactive_fg.hue()));

            break;
          }

          case Peaclock::Toggle::inactive_bg:
          {
            _peaclock.cfg.style.inactive_bg.hue(_peaclock.cfg.style.inactive_bg.hue() - 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.inactive_bg.hue()));

            break;
          }

          case Peaclock::Toggle::colon_fg:
          {
            _peaclock.cfg.style.colon_fg.hue(_peaclock.cfg.style.colon_fg.hue() - 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.colon_fg.hue()));

            break;
          }

          case Peaclock::Toggle::colon_bg:
          {
            _peaclock.cfg.style.colon_bg.hue(_peaclock.cfg.style.colon_bg.hue() - 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.colon_bg.hue()));

            break;
          }

          case Peaclock::Toggle::date:
          {
            _peaclock.cfg.style.date.hue(_peaclock.cfg.style.date.hue() - 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.date.hue()));

            break;
          }

          case Peaclock::Toggle::background:
          {
            _ctx.style.background.hue(_ctx.style.background.hue() - 0.5);
            _peaclock.cfg.style.background.hue(_peaclock.cfg.style.background.hue() - 0.5);
            set_status(true, "hue " + OB::String::to_string(_ctx.style.background.hue()));

            break;
          }

          default:
          {
            break;
          }
        }

        break;
      }

      case 'j':
      {
        switch (_peaclock.cfg.toggle)
        {
          case Peaclock::Toggle::block:
          {
            ++_peaclock.cfg.y_block;
            set_status(true, "block-y " + _peaclock.cfg.y_block.str());

            break;
          }

          case Peaclock::Toggle::padding:
          {
            ++_peaclock.cfg.y_space;
            set_status(true, "padding-y " + _peaclock.cfg.y_space.str());

            break;
          }

          case Peaclock::Toggle::margin:
          {
            ++_peaclock.cfg.y_border;
            set_status(true, "margin-y " + _peaclock.cfg.y_border.str());

            break;
          }

          case Peaclock::Toggle::ratio:
          {
            ++_peaclock.cfg.y_ratio;
            set_status(true, "ratio-y " + _peaclock.cfg.y_ratio.str());

            break;
          }

          case Peaclock::Toggle::active_fg:
          {
            _peaclock.cfg.style.active_fg.sat(_peaclock.cfg.style.active_fg.sat() + 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.active_fg.sat()));

            break;
          }

          case Peaclock::Toggle::active_bg:
          {
            _peaclock.cfg.style.active_bg.sat(_peaclock.cfg.style.active_bg.sat() + 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.active_bg.sat()));

            break;
          }

          case Peaclock::Toggle::inactive_fg:
          {
            _peaclock.cfg.style.inactive_fg.sat(_peaclock.cfg.style.inactive_fg.sat() + 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.inactive_fg.sat()));

            break;
          }

          case Peaclock::Toggle::inactive_bg:
          {
            _peaclock.cfg.style.inactive_bg.sat(_peaclock.cfg.style.inactive_bg.sat() + 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.inactive_bg.sat()));

            break;
          }

          case Peaclock::Toggle::colon_fg:
          {
            _peaclock.cfg.style.colon_fg.sat(_peaclock.cfg.style.colon_fg.sat() + 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.colon_fg.sat()));

            break;
          }

          case Peaclock::Toggle::colon_bg:
          {
            _peaclock.cfg.style.colon_bg.sat(_peaclock.cfg.style.colon_bg.sat() + 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.colon_bg.sat()));

            break;
          }

          case Peaclock::Toggle::date:
          {
            _peaclock.cfg.style.date.sat(_peaclock.cfg.style.date.sat() + 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.date.sat()));

            break;
          }

          case Peaclock::Toggle::background:
          {
            _ctx.style.background.sat(_ctx.style.background.sat() + 0.5);
            _peaclock.cfg.style.background.sat(_peaclock.cfg.style.background.sat() + 0.5);
            set_status(true, "sat " + OB::String::to_string(_ctx.style.background.sat()));

            break;
          }

          default:
          {
            break;
          }
        }

        break;
      }

      case 'k':
      {
        switch (_peaclock.cfg.toggle)
        {
          case Peaclock::Toggle::block:
          {
            --_peaclock.cfg.y_block;
            set_status(true, "block-y " + _peaclock.cfg.y_block.str());

            break;
          }

          case Peaclock::Toggle::padding:
          {
            --_peaclock.cfg.y_space;
            set_status(true, "padding-y " + _peaclock.cfg.y_space.str());

            break;
          }

          case Peaclock::Toggle::margin:
          {
            --_peaclock.cfg.y_border;
            set_status(true, "margin-y " + _peaclock.cfg.y_border.str());

            break;
          }

          case Peaclock::Toggle::ratio:
          {
            --_peaclock.cfg.y_ratio;
            set_status(true, "ratio-y " + _peaclock.cfg.y_ratio.str());

            break;
          }

          case Peaclock::Toggle::active_fg:
          {
            _peaclock.cfg.style.active_fg.sat(_peaclock.cfg.style.active_fg.sat() - 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.active_fg.sat()));

            break;
          }

          case Peaclock::Toggle::active_bg:
          {
            _peaclock.cfg.style.active_bg.sat(_peaclock.cfg.style.active_bg.sat() - 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.active_bg.sat()));

            break;
          }

          case Peaclock::Toggle::inactive_fg:
          {
            _peaclock.cfg.style.inactive_fg.sat(_peaclock.cfg.style.inactive_fg.sat() - 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.inactive_fg.sat()));

            break;
          }

          case Peaclock::Toggle::inactive_bg:
          {
            _peaclock.cfg.style.inactive_bg.sat(_peaclock.cfg.style.inactive_bg.sat() - 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.inactive_bg.sat()));

            break;
          }

          case Peaclock::Toggle::colon_fg:
          {
            _peaclock.cfg.style.colon_fg.sat(_peaclock.cfg.style.colon_fg.sat() - 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.colon_fg.sat()));

            break;
          }

          case Peaclock::Toggle::colon_bg:
          {
            _peaclock.cfg.style.colon_bg.sat(_peaclock.cfg.style.colon_bg.sat() - 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.colon_bg.sat()));

            break;
          }

          case Peaclock::Toggle::date:
          {
            _peaclock.cfg.style.date.sat(_peaclock.cfg.style.date.sat() - 0.5);
            set_status(true, "sat " + OB::String::to_string(_peaclock.cfg.style.date.sat()));

            break;
          }

          case Peaclock::Toggle::background:
          {
            _ctx.style.background.sat(_ctx.style.background.sat() - 0.5);
            _peaclock.cfg.style.background.sat(_peaclock.cfg.style.background.sat() - 0.5);
            set_status(true, "sat " + OB::String::to_string(_ctx.style.background.sat()));

            break;
          }

          default:
          {
            break;
          }
        }

        break;
      }

      case 'l':
      {
        switch (_peaclock.cfg.toggle)
        {
          case Peaclock::Toggle::block:
          {
            ++_peaclock.cfg.x_block;
            set_status(true, "block-x " + _peaclock.cfg.x_block.str());

            break;
          }

          case Peaclock::Toggle::padding:
          {
            ++_peaclock.cfg.x_space;
            set_status(true, "padding-x " + _peaclock.cfg.x_space.str());

            break;
          }

          case Peaclock::Toggle::margin:
          {
            ++_peaclock.cfg.x_border;
            set_status(true, "margin-x " + _peaclock.cfg.x_border.str());

            break;
          }

          case Peaclock::Toggle::ratio:
          {
            ++_peaclock.cfg.x_ratio;
            set_status(true, "ratio-x " + _peaclock.cfg.x_ratio.str());

            break;
          }

          case Peaclock::Toggle::active_fg:
          {
            _peaclock.cfg.style.active_fg.hue(_peaclock.cfg.style.active_fg.hue() + 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.active_fg.hue()));

            break;
          }

          case Peaclock::Toggle::active_bg:
          {
            _peaclock.cfg.style.active_bg.hue(_peaclock.cfg.style.active_bg.hue() + 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.active_bg.hue()));

            break;
          }

          case Peaclock::Toggle::inactive_fg:
          {
            _peaclock.cfg.style.inactive_fg.hue(_peaclock.cfg.style.inactive_fg.hue() + 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.inactive_fg.hue()));

            break;
          }

          case Peaclock::Toggle::inactive_bg:
          {
            _peaclock.cfg.style.inactive_bg.hue(_peaclock.cfg.style.inactive_bg.hue() + 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.inactive_bg.hue()));

            break;
          }

          case Peaclock::Toggle::colon_fg:
          {
            _peaclock.cfg.style.colon_fg.hue(_peaclock.cfg.style.colon_fg.hue() + 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.colon_fg.hue()));

            break;
          }

          case Peaclock::Toggle::colon_bg:
          {
            _peaclock.cfg.style.colon_bg.hue(_peaclock.cfg.style.colon_bg.hue() + 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.colon_bg.hue()));

            break;
          }

          case Peaclock::Toggle::date:
          {
            _peaclock.cfg.style.date.hue(_peaclock.cfg.style.date.hue() + 0.5);
            set_status(true, "hue " + OB::String::to_string(_peaclock.cfg.style.date.hue()));

            break;
          }

          case Peaclock::Toggle::background:
          {
            _ctx.style.background.hue(_ctx.style.background.hue() + 0.5);
            _peaclock.cfg.style.background.hue(_peaclock.cfg.style.background.hue() + 0.5);
            set_status(true, "hue " + OB::String::to_string(_ctx.style.background.hue()));

            break;
          }

          default:
          {
            break;
          }
        }

        break;
      }

      case ';':
      {
        switch (_peaclock.cfg.toggle)
        {
          case Peaclock::Toggle::active_fg:
          {
            _peaclock.cfg.style.active_fg.lum(_peaclock.cfg.style.active_fg.lum() - 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.active_fg.lum()));

            break;
          }

          case Peaclock::Toggle::active_bg:
          {
            _peaclock.cfg.style.active_bg.lum(_peaclock.cfg.style.active_bg.lum() - 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.active_bg.lum()));

            break;
          }

          case Peaclock::Toggle::inactive_fg:
          {
            _peaclock.cfg.style.inactive_fg.lum(_peaclock.cfg.style.inactive_fg.lum() - 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.inactive_fg.lum()));

            break;
          }

          case Peaclock::Toggle::inactive_bg:
          {
            _peaclock.cfg.style.inactive_bg.lum(_peaclock.cfg.style.inactive_bg.lum() - 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.inactive_bg.lum()));

            break;
          }

          case Peaclock::Toggle::colon_fg:
          {
            _peaclock.cfg.style.colon_fg.lum(_peaclock.cfg.style.colon_fg.lum() - 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.colon_fg.lum()));

            break;
          }

          case Peaclock::Toggle::colon_bg:
          {
            _peaclock.cfg.style.colon_bg.lum(_peaclock.cfg.style.colon_bg.lum() - 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.colon_bg.lum()));

            break;
          }

          case Peaclock::Toggle::date:
          {
            _peaclock.cfg.style.date.lum(_peaclock.cfg.style.date.lum() - 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.date.lum()));

            break;
          }

          case Peaclock::Toggle::background:
          {
            _ctx.style.background.lum(_ctx.style.background.lum() - 0.5);
            _peaclock.cfg.style.background.lum(_peaclock.cfg.style.background.lum() - 0.5);
            set_status(true, "lum " + OB::String::to_string(_ctx.style.background.lum()));

            break;
          }

          default:
          {
            break;
          }
        }

        break;
      }

      case '\'':
      {
        switch (_peaclock.cfg.toggle)
        {
          case Peaclock::Toggle::active_fg:
          {
            _peaclock.cfg.style.active_fg.lum(_peaclock.cfg.style.active_fg.lum() + 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.active_fg.lum()));

            break;
          }

          case Peaclock::Toggle::active_bg:
          {
            _peaclock.cfg.style.active_bg.lum(_peaclock.cfg.style.active_bg.lum() + 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.active_bg.lum()));

            break;
          }

          case Peaclock::Toggle::inactive_fg:
          {
            _peaclock.cfg.style.inactive_fg.lum(_peaclock.cfg.style.inactive_fg.lum() + 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.inactive_fg.lum()));

            break;
          }

          case Peaclock::Toggle::inactive_bg:
          {
            _peaclock.cfg.style.inactive_bg.lum(_peaclock.cfg.style.inactive_bg.lum() + 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.inactive_bg.lum()));

            break;
          }

          case Peaclock::Toggle::date:
          {
            _peaclock.cfg.style.date.lum(_peaclock.cfg.style.date.lum() + 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.date.lum()));

            break;
          }

          case Peaclock::Toggle::colon_fg:
          {
            _peaclock.cfg.style.colon_fg.lum(_peaclock.cfg.style.colon_fg.lum() + 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.colon_fg.lum()));

            break;
          }

          case Peaclock::Toggle::colon_bg:
          {
            _peaclock.cfg.style.colon_bg.lum(_peaclock.cfg.style.colon_bg.lum() + 0.5);
            set_status(true, "lum " + OB::String::to_string(_peaclock.cfg.style.colon_bg.lum()));

            break;
          }

          case Peaclock::Toggle::background:
          {
            _ctx.style.background.lum(_ctx.style.background.lum() + 0.5);
            _peaclock.cfg.style.background.lum(_peaclock.cfg.style.background.lum() + 0.5);
            set_status(true, "lum " + OB::String::to_string(_ctx.style.background.lum()));

            break;
          }

          default:
          {
            break;
          }
        }

        break;
      }

      case 'p':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::block;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'o':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::padding;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'i':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::margin;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'u':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::ratio;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'x':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::active_fg;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'c':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::inactive_fg;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'v':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::colon_fg;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'b':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::active_bg;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'n':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::inactive_bg;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'm':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::colon_bg;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case '.':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::background;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case ',':
      {
        _peaclock.cfg.toggle = Peaclock::Toggle::date;
        set_status(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));

        break;
      }

      case 'w':
      {
        _peaclock.cfg.mode = Peaclock::Mode::date;
        set_status(true, "mode " + Peaclock::Mode::str(_peaclock.cfg.mode));

        break;
      }

      case 'e':
      {
        _peaclock.cfg.mode = Peaclock::Mode::digital;
        set_status(true, "mode " + Peaclock::Mode::str(_peaclock.cfg.mode));

        break;
      }

      case 'r':
      {
        _peaclock.cfg.mode = Peaclock::Mode::binary;
        set_status(true, "mode " + Peaclock::Mode::str(_peaclock.cfg.mode));

        break;
      }

      case 't':
      {
        _peaclock.cfg.mode = Peaclock::Mode::icon;
        set_status(true, "mode " + Peaclock::Mode::str(_peaclock.cfg.mode));

        break;
      }

      default:
      {
        // ignore
        draw_keybuf();
        refresh();
        _ctx.keys.clear();

        return;
      }
    }

    clear();
    draw();
    refresh();
    _ctx.keys.clear();
  }

  while (OB::Term::get_key(&_ctx.key.str) > 0);
}

std::optional<std::pair<bool, std::string>> Tui::command(std::string const& input)
{
  // quit
  if (! _ctx.is_running)
  {
    _ctx.is_running = false;
    return {};
  }

  // nop
  if (input.empty())
  {
    return {};
  }

  auto const keys = OB::String::split(input, " ", 2);

  if (keys.empty())
  {
    return {};
  }

  // store the matches returned from OB::String::match
  std::optional<std::vector<std::string>> match_opt;

  // quit
  if (keys.size() == 1 && (keys.at(0) == "q" || keys.at(0) == "Q" ||
    keys.at(0) == "quit" || keys.at(0) == "Quit" || keys.at(0) == "exit"))
  {
    _ctx.is_running = false;
    return {};
  }

  else if (keys.at(0) == "rate-input" && (match_opt = OB::String::match(input,
    std::regex("^rate-input(?:\\s+([0-9]+))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "rate-input " + _ctx.input_interval.str());
    }
    else
    {
      _ctx.input_interval = std::stoi(match);
    }
  }

  else if (keys.at(0) == "rate-refresh" && (match_opt = OB::String::match(input,
    std::regex("^rate-refresh(?:\\s+([0-9]+))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "rate-refresh " + _ctx.refresh_rate.str());
    }
    else
    {
      _ctx.refresh_rate = std::stoi(match);
      _ctx.prompt.timeout = _ctx.prompt.rate.get() / _ctx.refresh_rate.get();
    }
  }

  else if (keys.at(0) == "rate-status" && (match_opt = OB::String::match(input,
    std::regex("^rate-status(?:\\s+([0-9]+))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "rate-status " + _ctx.prompt.rate.str());
    }
    else
    {
      _ctx.prompt.rate = std::stoi(match);
      _ctx.prompt.timeout = _ctx.prompt.rate.get() / _ctx.refresh_rate.get();
    }
  }

  else if (keys.at(0) == "locale" && (match_opt = OB::String::match(input,
    std::regex("^locale(?:\\s+(?:(" + _ctx.rx.str + ")))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "locale '" + _peaclock.cfg.locale + "'");
    }
    else
    {
      if (match.size() == 2)
      {
        _peaclock.cfg.locale = "";
      }
      else if (! _peaclock.cfg_locale(match.substr(1, match.size() - 2)))
      {
        return std::make_pair(false, "error: invalid locale '" + match + "'");
      }
    }
  }

  else if (keys.at(0) == "timezone" && (match_opt = OB::String::match(input,
    std::regex("^timezone(?:\\s+(?:(" + _ctx.rx.str + ")))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "timezone '" + _peaclock.cfg.timezone + "'");
    }
    else
    {
      if (match.size() == 2)
      {
        _peaclock.cfg.timezone = "";
      }
      else if (! _peaclock.cfg_timezone(match.substr(1, match.size() - 2)))
      {
        return std::make_pair(false, "error: invalid timezone '" + match + "'");
      }
    }
  }

  else if (keys.at(0) == "date" && (match_opt = OB::String::match(input,
    std::regex("^date(?:\\s+(?:(" + _ctx.rx.str + ")))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "date '" + OB::String::escape(_peaclock.cfg.datefmt) + "'");
    }
    else
    {
      if (match.size() == 2)
      {
        _peaclock.cfg.datefmt = "";
      }
      else
      {
        _peaclock.cfg_datefmt(OB::String::unescape(match.substr(1, match.size() - 2)));
      }
    }
  }

  else if (keys.at(0) == "fill" && (match_opt = OB::String::match(input,
    std::regex("^fill(?:\\s+(?:(" + _ctx.rx.str + ")))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "fill '" + OB::String::escape(_peaclock.cfg.fill_active) + "'");
    }
    else
    {
      if (match.size() == 2)
      {
        _peaclock.cfg.fill_active = "";
        _peaclock.cfg.fill_inactive = "";
        _peaclock.cfg.fill_colon = "";
      }
      else
      {
        _peaclock.cfg.fill_active = OB::String::unescape(match.substr(1, match.size() - 2));
        _peaclock.cfg.fill_inactive = _peaclock.cfg.fill_active;
        _peaclock.cfg.fill_colon = _peaclock.cfg.fill_active;
      }
    }
  }

  else if (keys.at(0) == "fill-active" && (match_opt = OB::String::match(input,
    std::regex("^fill-active(?:\\s+(?:(" + _ctx.rx.str + ")))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "fill-active '" + OB::String::escape(_peaclock.cfg.fill_active) + "'");
    }
    else
    {
      if (match.size() == 2)
      {
        _peaclock.cfg.fill_active = "";
      }
      else
      {
        _peaclock.cfg.fill_active = OB::String::unescape(match.substr(1, match.size() - 2));
      }
    }
  }

  else if (keys.at(0) == "fill-inactive" && (match_opt = OB::String::match(input,
    std::regex("^fill-inactive(?:\\s+(?:(" + _ctx.rx.str + ")))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "fill-inactive '" + OB::String::escape(_peaclock.cfg.fill_inactive) + "'");
    }
    else
    {
      if (match.size() == 2)
      {
        _peaclock.cfg.fill_inactive = "";
      }
      else
      {
        _peaclock.cfg.fill_inactive = OB::String::unescape(match.substr(1, match.size() - 2));
      }
    }
  }

  else if (keys.at(0) == "fill-colon" && (match_opt = OB::String::match(input,
    std::regex("^fill-colon(?:\\s+(?:(" + _ctx.rx.str + ")))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "fill-colon '" + OB::String::escape(_peaclock.cfg.fill_colon) + "'");
    }
    else
    {
      if (match.size() == 2)
      {
        _peaclock.cfg.fill_colon = "";
      }
      else
      {
        _peaclock.cfg.fill_colon = OB::String::unescape(match.substr(1, match.size() - 2));
      }
    }
  }

  else if (keys.at(0) == "mode" && (match_opt = OB::String::match(input,
    std::regex("^mode(?:\\s+(date|digital|binary|icon))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "mode " + Peaclock::Mode::str(_peaclock.cfg.mode));
    }
    else
    {
      _peaclock.cfg.mode = Peaclock::Mode::enm(match);
    }
  }

  else if (keys.at(0) == "toggle" && (match_opt = OB::String::match(input,
    std::regex("^toggle(?:\\s+(block|padding|margin|ratio|active-fg|inactive-fg|colon-fg|active-bg|inactive-bg|colon-bg|date|background))?$"))))
  {
    auto const match = match_opt.value().at(1);

    if (match.empty())
    {
      return std::make_pair(true, "toggle " + Peaclock::Toggle::str(_peaclock.cfg.toggle));
    }
    else
    {
      _peaclock.cfg.toggle = Peaclock::Toggle::enm(match);
    }
  }

  else if (keys.at(0) == "block" && (match_opt = OB::String::match(input,
    std::regex("^block(?:\\s+([0-9]+)\\s+([0-9]+))?$"))))
  {
    auto const x = match_opt.value().at(1);
    auto const y = match_opt.value().at(2);

    if (x.empty() && y.empty())
    {
      return std::make_pair(true, "block " + _peaclock.cfg.x_block.str() + " " + _peaclock.cfg.y_block.str());
    }
    else
    {
      _peaclock.cfg.x_block = std::stoul(x);
      _peaclock.cfg.y_block = std::stoul(y);
    }
  }

  else if (keys.at(0) == "block-x" && (match_opt = OB::String::match(input,
    std::regex("^block-x(?:\\s+([0-9]+))?$"))))
  {
    auto const x = match_opt.value().at(1);

    if (x.empty())
    {
      return std::make_pair(true, "block-x " + _peaclock.cfg.x_block.str());
    }
    else
    {
      _peaclock.cfg.x_block = std::stoul(x);
    }
  }

  else if (keys.at(0) == "block-y" && (match_opt = OB::String::match(input,
    std::regex("^block-y(?:\\s+([0-9]+))?$"))))
  {
    auto const y = match_opt.value().at(1);

    if (y.empty())
    {
      return std::make_pair(true, "block-y " + _peaclock.cfg.y_block.str());
    }
    else
    {
      _peaclock.cfg.y_block = std::stoul(y);
    }
  }

  else if (keys.at(0) == "padding" && (match_opt = OB::String::match(input,
    std::regex("^padding(?:\\s+([0-9]+)\\s+([0-9]+))?$"))))
  {
    auto const x = match_opt.value().at(1);
    auto const y = match_opt.value().at(2);

    if (x.empty() && y.empty())
    {
      return std::make_pair(true, "padding " + _peaclock.cfg.x_space.str() + " " + _peaclock.cfg.y_space.str());
    }
    else
    {
      _peaclock.cfg.x_space = std::stoul(x);
      _peaclock.cfg.y_space = std::stoul(y);
    }
  }

  else if (keys.at(0) == "padding-x" && (match_opt = OB::String::match(input,
    std::regex("^padding-x(?:\\s+([0-9]+))?$"))))
  {
    auto const x = match_opt.value().at(1);

    if (x.empty())
    {
      return std::make_pair(true, "padding-x " + _peaclock.cfg.x_space.str());
    }
    else
    {
      _peaclock.cfg.x_space = std::stoul(x);
    }
  }

  else if (keys.at(0) == "padding-y" && (match_opt = OB::String::match(input,
    std::regex("^padding-y(?:\\s+([0-9]+))?$"))))
  {
    auto const y = match_opt.value().at(1);

    if (y.empty())
    {
      return std::make_pair(true, "padding-y " + _peaclock.cfg.y_space.str());
    }
    else
    {
      _peaclock.cfg.y_space = std::stoul(y);
    }
  }

  else if (keys.at(0) == "margin" && (match_opt = OB::String::match(input,
    std::regex("^margin(?:\\s+([0-9]+)\\s+([0-9]+))?$"))))
  {
    auto const x = match_opt.value().at(1);
    auto const y = match_opt.value().at(2);

    if (x.empty() && y.empty())
    {
      return std::make_pair(true, "margin " + _peaclock.cfg.x_border.str() + " " + _peaclock.cfg.y_border.str());
    }
    else
    {
      _peaclock.cfg.x_border = std::stoul(x);
      _peaclock.cfg.y_border = std::stoul(y);
    }
  }

  else if (keys.at(0) == "margin-x" && (match_opt = OB::String::match(input,
    std::regex("^margin-x(?:\\s+([0-9]+))?$"))))
  {
    auto const x = match_opt.value().at(1);

    if (x.empty())
    {
      return std::make_pair(true, "margin-x " + _peaclock.cfg.x_border.str());
    }
    else
    {
      _peaclock.cfg.x_border = std::stoul(x);
    }
  }

  else if (keys.at(0) == "margin-y" && (match_opt = OB::String::match(input,
    std::regex("^margin-y(?:\\s+([0-9]+))?$"))))
  {
    auto const y = match_opt.value().at(1);

    if (y.empty())
    {
      return std::make_pair(true, "margin-y " + _peaclock.cfg.y_border.str());
    }
    else
    {
      _peaclock.cfg.y_border = std::stoul(y);
    }
  }

  else if (keys.at(0) == "ratio" && (match_opt = OB::String::match(input,
    std::regex("^ratio(?:\\s+([0-9]+)\\s+([0-9]+))?$"))))
  {
    auto const x = match_opt.value().at(1);
    auto const y = match_opt.value().at(2);

    if (x.empty() && y.empty())
    {
      return std::make_pair(true, "ratio " + _peaclock.cfg.x_ratio.str() + " " + _peaclock.cfg.y_ratio.str());
    }
    else
    {
      _peaclock.cfg.x_ratio = std::stoul(x);
      _peaclock.cfg.y_ratio = std::stoul(y);
    }
  }

  else if (keys.at(0) == "ratio-x" && (match_opt = OB::String::match(input,
    std::regex("^ratio-x(?:\\s+([0-9]+))?$"))))
  {
    auto const x = match_opt.value().at(1);

    if (x.empty())
    {
      return std::make_pair(true, "ratio-x " + _peaclock.cfg.x_ratio.str());
    }
    else
    {
      _peaclock.cfg.x_ratio = std::stoul(x);
    }
  }

  else if (keys.at(0) == "ratio-y" && (match_opt = OB::String::match(input,
    std::regex("^ratio-y(?:\\s+([0-9]+))?$"))))
  {
    auto const y = match_opt.value().at(1);

    if (y.empty())
    {
      return std::make_pair(true, "ratio-y " + _peaclock.cfg.y_ratio.str());
    }
    else
    {
      _peaclock.cfg.y_ratio = std::stoul(y);
    }
  }

  else if (keys.size() >= 2 && keys.at(0) == "style")
  {
    if (keys.at(1) == "active-fg")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style active-fg " + _peaclock.cfg.style.active_fg.key());
      }

      OB::Color color {keys.at(2)};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _peaclock.cfg.style.active_fg = color;
    }

    else if (keys.at(1) == "active-bg")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style active-bg " + _peaclock.cfg.style.active_bg.key());
      }

      OB::Color color {keys.at(2), OB::Color::Type::bg};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _peaclock.cfg.style.active_bg = color;
    }

    else if (keys.at(1) == "inactive-fg")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style inactive-fg " + _peaclock.cfg.style.inactive_fg.key());
      }

      OB::Color color {keys.at(2)};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _peaclock.cfg.style.inactive_fg = color;
    }

    else if (keys.at(1) == "inactive-bg")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style inactive-bg " + _peaclock.cfg.style.inactive_bg.key());
      }

      OB::Color color {keys.at(2), OB::Color::Type::bg};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _peaclock.cfg.style.inactive_bg = color;
    }

    else if (keys.at(1) == "colon-fg")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style colon-fg " + _peaclock.cfg.style.colon_fg.key());
      }

      OB::Color color {keys.at(2)};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _peaclock.cfg.style.colon_fg = color;
    }

    else if (keys.at(1) == "colon-bg")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style colon-bg " + _peaclock.cfg.style.colon_bg.key());
      }

      OB::Color color {keys.at(2), OB::Color::Type::bg};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _peaclock.cfg.style.colon_bg = color;
    }

    else if (keys.at(1) == "date")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style date " + _peaclock.cfg.style.date.key());
      }

      OB::Color color {keys.at(2)};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _peaclock.cfg.style.date = color;
    }

    else if (keys.at(1) == "text")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style text " + _ctx.style.text.key());
      }

      OB::Color color {keys.at(2)};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _ctx.style.text = color;
    }

    else if (keys.at(1) == "background")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style background " + _ctx.style.background.key());
      }

      OB::Color color {keys.at(2), OB::Color::Type::bg};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _ctx.style.background = color;
      _peaclock.cfg.style.background = color;
    }

    else if (keys.at(1) == "prompt")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style prompt " + _ctx.style.prompt.key());
      }

      OB::Color color {keys.at(2)};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _ctx.style.prompt = color;
    }

    else if (keys.at(1) == "success")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style success " + _ctx.style.success.key());
      }

      OB::Color color {keys.at(2)};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _ctx.style.success = color;
    }

    else if (keys.at(1) == "error")
    {
      if (keys.size() < 3)
      {
        return std::make_pair(true, "style error " + _ctx.style.error.key());
      }

      OB::Color color {keys.at(2)};

      if (! color)
      {
        return std::make_pair(false, "warning: unknown command '" + input + "'");
      }

      _ctx.style.error = color;
    }

    else
    {
      return std::make_pair(false, "warning: unknown command '" + input + "'");
    }
  }

  else if (keys.size() >= 2 && keys.at(0) == "set")
  {
    if (match_opt = OB::String::match(input,
      std::regex("^set\\s+date(?:\\s+(true|false|t|f|1|0|on|off))?$")))
    {
      auto const match = match_opt.value().at(1);

      if (match.empty())
      {
        return std::make_pair(true, "set date "s + btos(_peaclock.cfg.date));
      }
      else if ("true" == match || "t" == match || "1" == match || "on" == match)
      {
        _peaclock.cfg.date = true;
      }
      else
      {
        _peaclock.cfg.date = false;
      }
    }

    else if (match_opt = OB::String::match(input,
      std::regex("^set\\s+seconds(?:\\s+(true|false|t|f|1|0|on|off))?$")))
    {
      auto const match = match_opt.value().at(1);

      if (match.empty())
      {
        return std::make_pair(true, "set seconds "s + btos(_peaclock.cfg.seconds));
      }
      else if ("true" == match || "t" == match || "1" == match || "on" == match)
      {
        _peaclock.cfg.seconds = true;
      }
      else
      {
        _peaclock.cfg.seconds = false;
      }
    }

    else if (match_opt = OB::String::match(input,
      std::regex("^set\\s+hour-24(?:\\s+(true|false|t|f|1|0|on|off))?$")))
    {
      auto const match = match_opt.value().at(1);

      if (match.empty())
      {
        return std::make_pair(true, "set hour-24 "s + btos(_peaclock.cfg.hour_24));
      }
      else if ("true" == match || "t" == match || "1" == match || "on" == match)
      {
        _peaclock.cfg.hour_24 = true;
      }
      else
      {
        _peaclock.cfg.hour_24 = false;
      }
    }

    else if (match_opt = OB::String::match(input,
      std::regex("^set\\s+auto-size(?:\\s+(true|false|t|f|1|0|on|off))?$")))
    {
      auto const match = match_opt.value().at(1);

      if (match.empty())
      {
        return std::make_pair(true, "set auto-size "s + btos(_peaclock.cfg.auto_size));
      }
      else if ("true" == match || "t" == match || "1" == match || "on" == match)
      {
        _peaclock.cfg.auto_size = true;
      }
      else
      {
        _peaclock.cfg.auto_size = false;
      }
    }

    else if (match_opt = OB::String::match(input,
      std::regex("^set\\s+auto-ratio(?:\\s+(true|false|t|f|1|0|on|off))?$")))
    {
      auto const match = match_opt.value().at(1);

      if (match.empty())
      {
        return std::make_pair(true, "set auto-ratio "s + btos(_peaclock.cfg.auto_ratio));
      }
      else if ("true" == match || "t" == match || "1" == match || "on" == match)
      {
        _peaclock.cfg.auto_ratio = true;
      }
      else
      {
        _peaclock.cfg.auto_ratio = false;
      }
    }

    else
    {
      return std::make_pair(false, "warning: unknown command '" + input + "'");
    }
  }

  // unknown
  else
  {
    return std::make_pair(false, "warning: unknown command '" + input + "'");
  }

  return {};
}

void Tui::command_prompt()
{
  // reset prompt message count
  _ctx.prompt.count = 0;

  // set prompt style
  _readline.style(_ctx.style.text.value() + _ctx.style.background.value());
  _readline.prompt(":", _ctx.style.prompt.value() + _ctx.style.background.value());

  std::cout
  << aec::cursor_save
  << aec::cursor_set(0, _ctx.height)
  << aec::erase_line
  << aec::cursor_show
  << std::flush;

  // read user input
  auto input = _readline(_ctx.is_running);

  std::cout
  << aec::cursor_hide
  << aec::cursor_load
  << std::flush;

  if (auto const res = command(input))
  {
    set_status(res.value().first, res.value().second);
  }
}

int Tui::screen_size()
{
  bool width_invalid {_ctx.width < _ctx.width_min};
  bool height_invalid {_ctx.height < _ctx.height_min};

  if (width_invalid || height_invalid)
  {
    clear();

    _ctx.buf
    << _ctx.style.background
    << _ctx.style.error;

    if (width_invalid && height_invalid)
    {
      _ctx.buf
      << "Error: width "
      << _ctx.width
      << " (min "
      << _ctx.width_min
      << ") height "
      << _ctx.height
      << " (min "
      << _ctx.height_min
      << ")";
    }
    else if (width_invalid)
    {
      _ctx.buf
      << "Error: width "
      << _ctx.width
      << " (min "
      << _ctx.width_min
      << ")";
    }
    else
    {
      _ctx.buf
      << "Error: height "
      << _ctx.height
      << " (min "
      << _ctx.height_min
      << ")";
    }

    _ctx.buf
    << aec::clear;

    refresh();

    return 1;
  }

  return 0;
}
