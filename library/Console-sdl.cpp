/*
https://github.com/peterix/dfhack

A thread-safe logging console with a line editor.

Based on linenoise:
linenoise -- guerrilla line editing library against the idea that a
line editing lib needs to be 20,000 lines of C code.

You can find the latest source code at:

  http://github.com/antirez/linenoise

Does a number of crazy assumptions that happen to be true in 99.9999% of
the 2010 UNIX computers around.

------------------------------------------------------------------------

Copyright (c) 2010, Salvatore Sanfilippo <antirez at gmail dot com>
Copyright (c) 2010, Pieter Noordhuis <pcnoordhuis at gmail dot com>
Copyright (c) 2011, Petr Mrázek <peterix@gmail.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

 *  Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

 *  Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string.h>
#include <string>
#include <queue>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <errno.h>
#include <deque>
#include <thread>
#include <functional>
#include <memory>
#include <csignal>

#ifdef HAVE_CUCHAR
#include <cuchar>
#else
#include <cwchar>
#endif

// George Vulov for MacOSX
#ifndef __LINUX__
#define TMP_FAILURE_RETRY(expr) \
    ({ long int _res; \
        do _res = (long int) (expr); \
        while (_res == -1L && errno == EINTR); \
        _res; })
#endif

#include "Console.h"
extern "C" {
#include "SDL_console.h"
}
#include "Hooks.h"
using namespace DFHack;

#include "tinythread.h"
using namespace tthread;

namespace DFHack
{
    class Private
    {
    public:
        Private() {};
        virtual ~Private() {}
    private:

    public:
        bool init()
        {
            status = con_unclaimed;

            render_thread = std::thread([this] () {
                tty = Console_Create("DFHack Console", prompt.c_str(), 16);
                if (tty == nullptr) {
                    fprintf(stderr, "%s\n", Console_GetError());
                    status = con_shutdown;
                    status.notify_one();
                    return;
                }

                status = con_claimed;
                status.notify_one();

                Console_Draw(tty);
                Console_Destroy(tty);
            });

            status.wait(con_unclaimed);
            return is_running();
        }

        bool is_inited()
        {
            return status != con_unclaimed;
        }

        bool is_running()
        {
            return status == con_claimed;
        }

        bool is_shutdown()
        {
            return status == con_shutdown;
        }

        void shutdown()
        {
            if (!is_inited())
                return;

            status = con_shutdown;
            Console_Shutdown(tty);
            if (render_thread.joinable())
                render_thread.join();
            status = con_unclaimed;
        }

        void print(const char *data)
        {
            Console_AddLine(tty, data);
        }

        void print_text(color_ostream::color_value clr, const std::string &chunk)
        {
            print(chunk.c_str());
        }

        int lineedit(const std::string& prompt, std::string& output, recursive_mutex * lock, CommandHistory & ch)
        {
            if (is_shutdown())
                return Console::SHUTDOWN;

            if (prompt != this->prompt) {
                Console_SetPrompt(tty, prompt.c_str());
                this->prompt = prompt;
            }

          //  line_edit = true;
            int ret = Console_ReadLine(tty, output);
          //  line_edit = false;
            if (ret == 0)
                return Console::RETRY;
            return ret;
        }

        void begin_batch()
        {
        }

        void end_batch()
        {
        }

        void flush()
        {
        }

        /// Clear the console, along with its scrollback
        void clear()
        {
            if (!is_running())
                return;
            Console_Clear(tty);
        }
        /// Position cursor at x,y. 1,1 = top left corner
        void gotoxy(int x, int y)
        {
        }

        /// Set color (ANSI color number)
        void color(Console::color_value index)
        {
        }
        /// Reset color to default
        void reset_color(void)
        {
        }

        /// Enable or disable the caret/cursor
        void cursor(bool enable = true)
        {
        }

        /// Waits given number of milliseconds before continuing.
        void msleep(unsigned int msec);
        /// get the current number of columns
        int  get_columns(void)
        {
            return Console_GetColumns(tty);
        }
        /// get the current number of rows
        int  get_rows(void)
        {
            return Console_GetRows(tty);
        }


        std::thread render_thread;
        Console_tty *tty{nullptr};
        FILE * dfout_C{nullptr};
        // current state
        enum console_state
        {
            con_unclaimed,
            con_claimed,
            con_shutdown
        } state;
        bool in_batch{false};
        std::string prompt;      // current prompt string
        std::atomic<int> status{con_unclaimed};
    };
}

Console::Console()
{
    d = 0;
    inited = false;
    // we can't create the mutex at this time. the SDL functions aren't hooked yet.
    wlock = new recursive_mutex();
}
Console::~Console()
{
    assert(!inited);
    if(wlock)
        delete wlock;
    if(d)
        delete d;
}

/*
 * We can't wait for the SDL console render thread
 * to spin up here else we'll deadlock. So we'll lie and say
 * inited is true. We can queue any operations
 * that make sense for queuing while we wait.
 *
 */
bool Console::init(bool dont_redirect)
{
   // lock_guard <recursive_mutex> g(*wlock);
    d = new Private();
    inited = d->init();
    return inited;

    // TODO: redirect streams

    // make our own weird streams so our IO isn't redirected
    if (dont_redirect)
    {
        d->dfout_C = fopen("/dev/stdout", "w");
    }
    else
    {
        if (!freopen("stdout.log", "w", stdout))
            ;
        d->dfout_C = fopen("/dev/tty", "w");
        if (!d->dfout_C)
        {
            fprintf(stderr, "could not open tty\n");
            d->dfout_C = fopen("/dev/stdout", "w");
            return false;
        }
    }
    std::cin.tie(this);
    clear();
    return true;
}

bool Console::shutdown(void)
{
    lock_guard <recursive_mutex> g(*wlock);
    if(!d || !inited)
        return true;
    d->shutdown();
    inited = false;
    return true;
}

/* XXX: Not implemented */
void Console::begin_batch()
{
    wlock->lock();

    if (inited)
        d->begin_batch();
}
/* XXX: Not implemented */
void Console::end_batch()
{
    if (inited)
        d->end_batch();

    wlock->unlock();
}
/* XXX: Not implemented */
void Console::flush_proxy()
{
    return;
    lock_guard <recursive_mutex> g(*wlock);
    if (inited)
        d->flush();
}
/* XXX: color not implemented */
void Console::add_text(color_value color, const std::string &text)
{
    lock_guard <recursive_mutex> g(*wlock);
    if (inited)
        d->print_text(color, text);
    else
        fwrite(text.data(), 1, text.size(), stderr);
}

int Console::get_columns(void)
{
    lock_guard <recursive_mutex> g(*wlock);
    int ret = Console::FAILURE;
    if(inited) {
        if (d->is_running())
            ret = d->get_columns();
        else
            return Console::RETRY;
    }
    return ret;
}

int Console::get_rows(void)
{
    lock_guard <recursive_mutex> g(*wlock);
    int ret = Console::FAILURE;
    if(inited) {
        if (d->is_running())
            ret = d->get_rows();
        else
            return Console::RETRY;
    }
    return ret;
}

void Console::clear()
{
    lock_guard <recursive_mutex> g(*wlock);
    d->clear();
}
/* XXX: Not implemented */
void Console::gotoxy(int x, int y)
{
    lock_guard <recursive_mutex> g(*wlock);
    if(inited)
        d->gotoxy(x,y);
}
/* XXX: Not implemented */
void Console::cursor(bool enable)
{
    lock_guard <recursive_mutex> g(*wlock);
    if(inited)
        d->cursor(enable);
}

int Console::lineedit(const std::string & prompt, std::string & output, CommandHistory & ch)
{
    //lock_guard <recursive_mutex> g(*wlock);
    /* wlock here results in a deadlock */
    if (!inited)
        return Console::SHUTDOWN;

    int ret = d->lineedit(prompt,output,wlock,ch);
    if (ret == Console::SHUTDOWN)
        inited = false;
    else if (ret == 0)
        return Console::RETRY;
    return ret;
}

void Console::msleep (unsigned int msec)
{
    if (msec > 1000) sleep(msec/1000000);
    usleep((msec % 1000000) * 1000);
}
/* XXX: Not implemented */
bool Console::hide()
{
    //Warmist: don't know if it's possible...
    return false;
}
/* XXX: Not implemented */
bool Console::show()
{
    //Warmist: don't know if it's possible...
    return false;
}

/*
bool Console::sdl_event(void* event)
{
    return false;
    if (!d->is_running())
        return false;
}*/

