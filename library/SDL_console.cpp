#define GL_GLEXT_PROTOTYPES 1
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <stdbool.h>
#include <assert.h>
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glu.h>
#include <ft2build.h>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <functional>
#include <codecvt>
#include <locale>
#include <vector>
#include <condition_variable>
#include <cstring>

#include FT_FREETYPE_H
#include "SDL_console.h"
#include "SDL_console_font.h"

static const GLchar* _Console_vertex_source =
    "#version 130\n"
    "in vec4 vertex; // <vec2 pos, vec2 tex>\n"
    "out vec2 TexCoords;\n"
    "uniform mat4 projection;\n"
    "void main()\n"
    "{\n"
    "   gl_Position = projection * vec4(vertex.xy, 0.0, 1.0);\n"
    "   TexCoords = vertex.zw;\n"
    "}";
static const GLchar* _Console_frag_source =
    "#version 130\n"
    "in vec2 TexCoords;\n"
    "out vec4 outColor;\n"
    "uniform sampler2D text;\n"
    "uniform vec3 textColor;\n"
    "void main()\n"
    "{\n"
    "   vec4 sampled = vec4(1.0, 1.0, 1.0, texture2D(text, TexCoords).r);\n"
    "   outColor = vec4(textColor, 1.0) * sampled;\n"
    "}";

static constexpr uint8_t num_tty = 1;
static const std::u32string default_prompt = U"> ";
static constexpr size_t default_max_lines = 512;
static constexpr size_t border_margin = 20;
static std::string _Console_errstr;

void console_set_error(std::string reason, std::string errmsg) {
    _Console_errstr = reason + errmsg;
}

const char *
Console_GetError (void)
{
    return _Console_errstr.c_str();
}

const char *
FT_GetError (FT_Error err)
{
    #undef __FTERRORS_H__
    #define FT_ERRORDEF(e,v,s)   case e: return s;
    #define FT_ERROR_START_LIST  switch (err) {
    #define FT_ERROR_END_LIST    }
    #include FT_ERRORS_H
    return "(Uknown Error)";

}


namespace SDLConsole {
enum class ViewUpdate { resize, scroll };
enum class NavDirection { up, down, page_up, page_down };
enum class State { claimed, shutdown, unclaimed };
enum class EventType { sdl, api };
enum class LineType { input, output };

struct Console_Line {
    std::u32string text;
    LineType type;
    GLfloat w{0};
    GLfloat h{0};
    int     y{0};
    bool    selected{false};
    GLuint texture{0};
    Console_Line *next;
    Console_Line *prev;
};

struct Console_Font {
    FT_Library ft;
    FT_Face face;
    GLint font_size;
    GLint char_width;
    GLfloat advance;
    GLfloat line_height;
    GLfloat baseline;
};

struct Console_Prompt {
    std::u32string prompt_text;
    std::u32string input;
    bool rebuild{true};
    size_t cursor{0};     /* position of cursor within curr_history */
    GLfloat w{0};
    GLfloat h{0};
    GLuint texture{0};
    /* 1x1 textures which hold the opacity values for the cursor & bg color */
    GLuint cursor_texture{0};
};

typedef std::function<void()> ApiCall;
class EventQueue {
public:
    void push_sdl(SDL_Event event) {
        std::unique_lock lock(sdl_mutex);
        sdl_queue.push(event);
        got_some = true;
        lock.unlock();
        got_some.notify_one();
    }

    void push_api(ApiCall func) {
        std::unique_lock lock(api_mutex);
        api_queue.push(std::move(func));
        got_some = true;
        lock.unlock();
        got_some.notify_one();
      }

    bool pop_sdl(SDL_Event& event) {
        std::scoped_lock lock(sdl_mutex);
        if (!sdl_queue.empty()) {
            event = sdl_queue.front();
            sdl_queue.pop();
            return true;
        }
        return false;
    }

    bool pop_api(ApiCall& func) {
        std::scoped_lock lock(api_mutex);
        if (!api_queue.empty()) {
            func = api_queue.front();
            api_queue.pop();
            return true;
        }
        return false;
    }

    void wait_for_events() {
        got_some.wait(false);
        {
            std::scoped_lock lock(sdl_mutex,  api_mutex);
            got_some = false;
        }
    }

private:
    std::mutex sdl_mutex;
    std::mutex api_mutex;
    std::queue<SDL_Event> sdl_queue;
    std::queue<ApiCall> api_queue;
    std::atomic<bool> got_some{false};
};

static std::recursive_mutex mutex;
}

using namespace SDLConsole;
struct _SDL_console_tty {
    Console_Font font;
    Console_Prompt prompt;

    SDL_Window *window{nullptr};
    int window_width{640};
    int window_height{480};

    Console_Color bg_color;
    Console_Color font_color;

    GLuint VAO{0};
    GLuint VBO{0};
    GLuint shader_prog{0};
    GLuint vert_shader{0};
    GLuint frag_shader{0};
    GLuint bg_texture{0};
    SDL_GLContext gl_context{nullptr};

    /* doubly linked-list of lines */
    Console_Line *lines_head{nullptr};
    Console_Line *lines_tail{nullptr};
    /* For input history */
    Console_Line *curr_history{nullptr};

    int scroll_offset{0};

    size_t num_lines{0};  /* current number of lines */
    size_t max_lines{default_max_lines};  /* max numbers of lines allowed */
    size_t wrap_len{0};   /* the number of characters when line should wrap */


    std::queue<std::u32string> input_complete_q;
    std::atomic<bool> cv_input_completed;
    std::thread::id render_thread_id;
    EventQueue event_q;

    std::atomic<State> status{State::unclaimed};
};

static Console_tty tty_list[num_tty];

void console_on_view_update(Console_tty *, ViewUpdate);

static std::u32string from_utf8(const char *str) {
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> c;
    return c.from_bytes(str);
}

static std::string to_utf8(std::u32string &u_str) {
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> c;
    return c.to_bytes(u_str);
}

int
console_update_line_texture(
        Console_tty *tty,
        const GLuint texture,
        const std::u32string &text,
        const int w,
        const int h)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    /* Resize the texture if needed and set any attributes */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    /* set of `empty' pixels to clear texture */
    unsigned char empty[(int)(w * h)];
    memset(empty, 0, (int)(w * h));

    /* Clear the texture */
    glTexSubImage2D(GL_TEXTURE_2D,
            0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, empty);

    const Console_Font *font = &tty->font;
    GLfloat advance = font->char_width;
    GLfloat x = 0, y = 0;

    for (auto &ch : text) {
        if (FT_Load_Char(font->face, ch, FT_LOAD_RENDER))
            continue;

        GLfloat bearingY = font->face->glyph->bitmap_top;

        if (x + advance > w || ch == '\n') {
            y += font->line_height;
            x = 0.0f;

            if (ch == '\n')
                continue;
        }

        if (ch != ' ') {
            /*
             * Every character has a different bearing. To account for that we use
             * the current line (y) and add in the line height as a buffer. In that
             * buffer can each character be placed at different y values so they
             * all appear in the same baseline.
             */
            GLfloat ypos = y + font->line_height - bearingY - font->baseline - 1.0f;

            /*
             * We also make sure to use SubImage here because we're actually
             * appending to the previously created texture.
             */
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                x,
                ypos,
                font->face->glyph->bitmap.width,
                font->face->glyph->bitmap.rows,
                GL_RED,
                GL_UNSIGNED_BYTE,
                font->face->glyph->bitmap.buffer
            );
        }

        x += advance;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

/*
 * Updates the texture given with the input and prompt and also the output, if
 * not null. This *will* changed the values w & h with the width and height of
 * the texture as the input, prompt, and output change lengths to vary the size
 * of the texture.
 */
int
console_update_io_texture (
        Console_tty *tty,
        Console_Line *line,
        Console_Line *texture_line)
{
    assert(tty);
    assert(line);
    assert(texture_line);

    Console_Font &font = tty->font;
    int len = 0;
    /* string buffer to output characters from */
    std::u32string str;

    if (line->type == LineType::input) {
        len = tty->prompt.prompt_text.length();
        str = tty->prompt.prompt_text;
    }
    len += line->text.length();
    str += line->text;

    line->w = tty->wrap_len * font.char_width;
    line->h = (ceil((float)len / (float)tty->wrap_len)) * font.line_height;

    return console_update_line_texture(tty, line->texture, str, line->w, line->h);
}

int
console_update_prompt_texture (Console_tty *tty)
{
    assert(tty);

    auto &line = tty->prompt;
    Console_Font &font = tty->font;

    auto str = line.prompt_text;
    if (false == line.input.empty())
        str += line.input;

    const int len = str.length();
    line.w = tty->wrap_len * font.char_width;
    line.h = (ceil((float)len / (float)tty->wrap_len)) * font.line_height;

    return console_update_line_texture(tty, line.texture, str, line.w, line.h);
}

void
console_render_texture (Console_tty *tty,
                        const uint texture,
                        const float x,
                        const float y,
                        const float w,
                        const float h)
{
    GLfloat vertices[6][4] = {
            { x,        y + h,  0.0f, 0.0f},
            { x,        y,      0.0f, 1.0f},
            { x + w,    y,      1.0f, 1.0f},

            { x,        y + h,  0.0f, 0.0f},
            { x + w,    y,      1.0f, 1.0f},
            { x + w,    y + h,  1.0f, 0.0f}
        };
    glBindTexture(GL_TEXTURE_2D, texture);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void
console_render_background (Console_tty *tty)
{
    const GLfloat xpos = 0.0f;
    const GLfloat ypos = 0.0f;
    const GLfloat ww   = tty->window_width;
    const GLfloat wh   = tty->window_height+(tty->scroll_offset * tty->font.line_height);
    glUniform3f(glGetUniformLocation(tty->shader_prog, "textColor"),
            tty->bg_color.r, tty->bg_color.g, tty->bg_color.b);
    console_render_texture(tty, tty->bg_texture, xpos, ypos, ww, wh);
}

void
console_render_background_line (Console_tty *tty, Console_Line *l)
{
    const GLfloat xpos = 0.0f;
    const GLfloat ypos = l->y;
    const GLfloat ww   = l->w;
    const GLfloat wh   = l->h;

    glUniform3f(glGetUniformLocation(tty->shader_prog, "textColor"),
            0.2f, 0.2f, 0.2f);
    console_render_texture(tty, tty->bg_texture, xpos, ypos, ww, wh);
}

void
console_on_mouse_button_down (Console_tty *tty, SDL_MouseButtonEvent& b)
{
    if (b.button == SDL_BUTTON_RIGHT) {
        for (Console_Line *l = tty->lines_head; l; l = l->next) {
            l->selected = false;
        }
        return;
    } else if (b.button != SDL_BUTTON_LEFT) {
        return;
    }

    int y = tty->window_height - b.y;
    y = y + (tty->scroll_offset * tty->font.line_height);

    for (Console_Line *l = tty->lines_head; l; l = l->next) {
        if (l->y < y && l->y + l->h > y) {
            l->selected = !l->selected;
            break;
        }
    }
}

void
console_render_cursor (Console_tty *tty)
{
    /* cursor's position */
    const int cursor_len  = tty->prompt.cursor + tty->prompt.prompt_text.length();
    const GLfloat lh = tty->font.line_height;
    const GLfloat cw = tty->font.char_width;
    /* the cursor's length within the line wrapped by max line characters */
    const GLfloat cx = (float)((cursor_len % tty->wrap_len) * tty->font.char_width);
    // note: don't cast cursor_len itself to float
    const GLfloat cy = ((tty->prompt.h / lh) -
                 (float)((cursor_len / tty->wrap_len) + 1)) * lh;

    /* Draw the cursor */
    glUniform3f(glGetUniformLocation(tty->shader_prog, "textColor"),
            tty->font_color.r, tty->font_color.g, tty->font_color.b);
    console_render_texture(tty, tty->prompt.cursor_texture, cx, cy, cw, lh);
}

void
console_render_textures (Console_tty *tty)
{
    Console_Line *line;
    GLfloat xpos = 0.0f;
    GLfloat ypos = 0.0f;
    const GLint uniformLoc = glGetUniformLocation(tty->shader_prog, "textColor");
    const int max_h = tty->window_height + (tty->scroll_offset*2*tty->font.line_height);
    int offset_h = (tty->scroll_offset * (int)tty->font.line_height);

    glUniform3f(uniformLoc, 1.0f, 1.0f, 1.0f);

    const Console_Prompt &pl = tty->prompt;
    if (offset_h <= pl.h) {
        console_render_texture(tty, pl.texture, xpos, ypos, pl.w, pl.h);
        ypos += pl.h;
    }

    int cur_h = 0;
    for (line = tty->lines_head; line; line = line->next) {
        cur_h += line->h;
        if (cur_h > offset_h) {
            break;
        }
    }

    if (!line) return;

    cur_h = 0;
    for (; line; cur_h += line->h, line = line->next) {
        if (line->selected) {
            console_render_background_line(tty, line);
            glUniform3f(uniformLoc, 1.0f, 1.0f, 1.0f);
        }

        console_render_texture(tty, line->texture, xpos, ypos, line->w, line->h);
        // record y position of this line
        line->y = ypos;
        ypos += line->h;

        if (cur_h > max_h)
            break;
    }
}

int
console_render (Console_tty *tty)
{
    assert(tty);


    glUseProgram(tty->shader_prog);

       /* set all options, programs, and buffers to draw our lines an d cursor */
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindVertexArray(tty->VAO);
    glBindBuffer(GL_ARRAY_BUFFER, tty->VBO);
    glActiveTexture(GL_TEXTURE0);

    /* set buffer size */
    glBufferData(GL_ARRAY_BUFFER, 
            sizeof(GLfloat) * 6 * 4, NULL, GL_DYNAMIC_DRAW);

    if (tty->prompt.rebuild) {
        tty->prompt.rebuild = false;
        if (console_update_prompt_texture(tty))
            return 1;
    }

    glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, tty->window_width, tty->window_height);
    glScissor(0, 0,tty->window_width,tty->window_height);
    console_render_background(tty);

    /* render text area */
    glViewport(border_margin/2, border_margin/2,
               tty->window_width-(border_margin*2), tty->window_height-border_margin);
    console_render_textures(tty);
    console_render_cursor(tty);

    /* unset all of the stuff we set
     * we need to unset here because the scrollbar placeholder
     * isn't compatible with textures.
     */

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D);

#if 0
    /* render scrollbar placeholder */
    int button_margin = 5;
    glViewport(tty->window_width-border_margin, border_margin+button_margin,
               tty->window_width, tty->window_height-(border_margin*2+button_margin*2));
    glScissor(tty->window_width-border_margin, border_margin+button_margin,
              tty->window_width, tty->window_height-(border_margin*2+button_margin*2));
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0.5f, 0.0f, 1.0f);
    glRectf(-1.0f,1.0f, 1.0f, -1.0f);

    /* up button */
    glViewport(tty->window_width-border_margin, tty->window_height-border_margin, tty->window_width, border_margin);
    glScissor(tty->window_width-border_margin, tty->window_height-border_margin, tty->window_width, border_margin);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0.5f, 0.0f, 1.0f);
    glRectf(-1.0f,1.0f, 1.0f, -1.0f);

    /* down button */
    glViewport(tty->window_width-border_margin, 0, tty->window_width, border_margin);
    glScissor(tty->window_width-border_margin, 0, tty->window_width, border_margin);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0.5f, 0.0f, 1.0f);
    glRectf(-1.0f,1.0f, 1.0f, -1.0f);
#endif

    SDL_GL_SwapWindow(tty->window);
    return 0;
}

/*
 * Set the current line. We can go UP (next) or DOWN (previous) through the 
 * lines. This function essentially acts as a history viewer. This function
 * will skip lines with zero length. The cursor is always set to the length of
 * the line's input.
 */

void
console_update_prompt_from_history (Console_tty *tty, NavDirection dir)
{
    /* no history */
    if (!tty->curr_history)
        return;

    Console_Line *line = (dir == NavDirection::up ? tty->curr_history->next : tty->curr_history->prev);
    for (; line != nullptr; line = (dir == NavDirection::up ? line->next : line->prev)) {
        if (line->type == LineType::input && !line->text.empty())
            break;
    }

    if (line == nullptr)
        return;

    tty->curr_history = line;
    tty->prompt.input = line->text;
    tty->prompt.cursor = line->text.length();
    tty->prompt.rebuild = true;
}

void
console_on_scroll (Console_tty *tty, const NavDirection dir)
{
    switch(dir) {
        case NavDirection::up:
            tty->scroll_offset += 1;
            break;
        case NavDirection::down:
            tty->scroll_offset -= 1;
            break;
        case NavDirection::page_up:
            tty->scroll_offset += Console_GetRows(tty) / 2;
            break;
        case NavDirection::page_down:
            tty->scroll_offset -= Console_GetRows(tty) / 2;
            break;
    }

    tty->scroll_offset = std::max(0, tty->scroll_offset);
    console_on_view_update(tty, ViewUpdate::scroll);
}

/*
 * Create a new line and set it to be the head. This function will 
 * automatically cycle-out lines if the number of lines has reached the max.
 * If this function returns 0, the tty->lines_head will be a new line and 
 * tty->lines_head->next is previous line.
 */
Console_Line *
console_create_line (Console_tty *tty,
                     const LineType line_type,
                     const std::u32string text)
{
    assert(tty);
    Console_Line *line = new Console_Line();
    if (!line) {
        console_set_error("Not enough memory to create line!", "");
        return nullptr;
    }

    glGenTextures(1, &line->texture);
    line->type = line_type;
    line->text = text;

    /* insert into the doubly-linked list */
    if (tty->lines_head == nullptr) {
        line->next = nullptr;
        line->prev = nullptr;
        tty->lines_head = line;
        tty->lines_tail = line;
        tty->num_lines  = 0;
    } else {
        line->prev = nullptr;
        line->next = tty->lines_head;
        tty->lines_head->prev = line;
        tty->lines_head = line;
    }

    /* When the list is too long, start chopping the tail off each new line */
    if (tty->num_lines == tty->max_lines) {
        tty->lines_tail = tty->lines_tail->prev;
        glDeleteTextures(1, &tty->lines_tail->next->texture);
        delete tty->lines_tail->next;
        tty->lines_tail->next = nullptr;
    } else {
        tty->num_lines++;
    }

    /* make sure the current line becomes the new line */
    // XXX:
    tty->curr_history = tty->lines_head;
    return line;
}

int
console_on_new_input_line (Console_tty *tty, const std::u32string text)
{
    Console_Line *l = console_create_line(tty, LineType::input, std::move(text));
    if (!l || console_update_io_texture(tty, l, l))
        return 1;

    {
        std::scoped_lock lock(mutex);
        tty->input_complete_q.push(tty->prompt.input);
        tty->cv_input_completed = true;
    }
    tty->cv_input_completed.notify_one();

    tty->prompt.input.clear();
    tty->prompt.cursor = 0;
    tty->prompt.rebuild = true;
    return 0;
}

int
console_on_new_output_line (Console_tty *tty, const std::u32string text)
{
    Console_Line *l = console_create_line(tty, LineType::output, std::move(text));
    if (!l || console_update_io_texture(tty, l, l))
        return 1;

    return 0;
}

void
console_destroy_ft  (Console_tty *tty)
{
    assert(tty);
    FT_Done_Face(tty->font.face);
    FT_Done_FreeType(tty->font.ft);
}

/*
 * Initialize the font.
 * TODO-FEATURE: extract the New_Face and Done_Face functions so we can change
 * fonts at run time.
 */
//int
//console_init_ft (Console_tty* tty, const char *font_path, const int font_size)
int
console_init_ft (Console_tty* tty, const int font_size)
{
    assert(tty);

    FT_Library ft;
    FT_Face face;
    FT_Error e;

    if ((e = FT_Init_FreeType(&ft))) {
        console_set_error("Freetype failed to init: ", FT_GetError(e));
        return 1;
    }

    /*
    if ((e = FT_New_Face(ft, font_path, 0, &face))) {
        console_set_error("Freetype failed open font: ", FT_GetError(e));
        FT_Done_FreeType(ft);
        return 1;
    }*/

    if ((e = FT_New_Memory_Face(ft, SDL_console_font, SDL_console_font_len, 0, &face))) {
        console_set_error("Freetype failed open font: ", FT_GetError(e));
        FT_Done_FreeType(ft);
        return 1;
    }

    tty->font.ft   = ft;
    tty->font.face = face;

    if (!FT_IS_FIXED_WIDTH(face)) {
        console_set_error("Font must be fixed width (monospace)!", "");
        console_destroy_ft(tty);
        return 1;
    }

    if (!FT_IS_SCALABLE(face)) {
        console_set_error("Font isn't scalable!", "");
        console_destroy_ft(tty);
        return 1;
    }

    FT_Set_Pixel_Sizes(face, 0, font_size);


    if ((e = FT_Load_Glyph(face, FT_Get_Char_Index(face, 'm'), 
            FT_LOAD_RENDER))) {
        console_set_error("Loading glyphs failed: ", FT_GetError(e));
        console_destroy_ft(tty);
        return 1;
    }

    /* `>> 6' adjusts values which are based at 1/64th of screen pixel size */
    tty->font.face = face;
    tty->font.ft = ft;
    tty->font.font_size = font_size;
    tty->font.advance = (face->glyph->metrics.horiAdvance >> 6);
    tty->font.char_width = 
        (face->glyph->metrics.horiBearingX + face->glyph->metrics.width) >> 6;
    tty->font.line_height = 
          (FT_MulFix(face->ascender, face->size->metrics.y_scale) >> 6)
        - (FT_MulFix(face->descender, face->size->metrics.y_scale) >> 6)
        + 1;
    tty->font.baseline = abs(face->descender) * font_size / face->units_per_EM;

    return 0;
}

void
console_destroy_gl (Console_tty *tty)
{
    assert(tty);
	glDeleteTextures(1, &tty->prompt.cursor_texture);
    glDeleteTextures(1, &tty->prompt.texture);
	glDeleteTextures(1, &tty->bg_texture);
    glDeleteShader(tty->vert_shader);
    glDeleteShader(tty->frag_shader);
    glDeleteProgram(tty->shader_prog);
	glDeleteBuffers(1, &tty->VBO);
	glDeleteBuffers(1, &tty->VAO);
}

/*
 * Get the window's size and all the vars that are associated with the window
 * size. Called for resize and scroll events.
 */
void
console_on_view_update (Console_tty *tty, const ViewUpdate type)
{
    assert(tty);
    assert(tty->window);
    assert(tty->shader_prog > 0);

    glUseProgram(tty->shader_prog);
    if (type == ViewUpdate::resize) {
        SDL_GetWindowSize(tty->window, &tty->window_width, &tty->window_height);

        /* wrap len needs to be updated before updating textures */
        tty->wrap_len =
            roundf((float)tty->window_width / (float)tty->font.char_width);


        console_update_prompt_texture(tty);
        /* XXX: we probably don't need to update textures outside visible view */
        Console_Line *li;
        for (li = tty->lines_head; li != nullptr; li = li->next)
            console_update_io_texture(tty, li, li);

        glViewport(0, 0, tty->window_width, tty->window_height);
    }

    /* 
     * Setup 2D projection matrix for vertex shader.
     * left, right, bottom, top
     *
     * Used when scrolling and scaling.
     */
    const GLfloat l = 0.0f;
    const GLfloat r = (GLfloat)tty->window_width;
    const GLfloat offset = tty->scroll_offset * tty->font.line_height;
    const GLfloat b = offset;
    const GLfloat t = (GLfloat)tty->window_height + offset;

    GLfloat orthoMatrix[4*4] = {
        2.0f / (r - l),   0.0f,             0.0f, 0.0f,
        0.0f,             2.0f / (t - b),   0.0f, 0.0f,
        0.0f,             0.0f,            -1.0f, 0.0f,
        -(r + l)/(r - l), -(t + b )/(t - b), 0.0f, 1.0f,
    };
    glUniformMatrix4fv(glGetUniformLocation(tty->shader_prog, "projection"),
           1, GL_FALSE, orthoMatrix);
    glUseProgram(0);
}

/*
 * Load all of the OpenGL specific aspects of the tty. This includes the 
 * cursor's texture. The only things it doesn't handle is the texture of each
 * line (handled by console_on_new_input_line) and the cursor's texture
 * (_Console_init_cursor). console_destroy_gl cleans up both of those.
 */
int
console_init_gl (Console_tty *tty)
{
    assert(tty);

    GLuint VAO;
    GLuint VBO;
    GLuint shader_prog;
    GLuint vert_shader;
    GLuint frag_shader;

    GLint  maxlength;
    GLint  status;
    GLint  posAttrib;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
            SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

    glGenVertexArrays(1, &VAO);
	glGenBuffers(1, &VBO);

#define _Console_m_compile_shader(s,src,t) \
    s = glCreateShader(t); \
    glShaderSource(s, 1, &src, NULL); \
    glCompileShader(s); \
    glGetShaderiv(s, GL_COMPILE_STATUS, &status); \
    if (status != GL_TRUE) { \
        char buffer[512]; \
        glGetShaderInfoLog(s, 512, NULL, buffer); \
        return 1; \
    }

    _Console_m_compile_shader(vert_shader,
            _Console_vertex_source, GL_VERTEX_SHADER);
    _Console_m_compile_shader(frag_shader,
            _Console_frag_source, GL_FRAGMENT_SHADER);
    shader_prog = glCreateProgram();

    /* setup values so they can be used to in destroy function if needed */
    tty->VAO = VAO;
    tty->VBO = VBO;
    tty->shader_prog = shader_prog;
    tty->vert_shader = vert_shader;
    tty->frag_shader = frag_shader;

    /* actually link shaders */
    glAttachShader(shader_prog, vert_shader);
    glAttachShader(shader_prog, frag_shader);
    glBindFragDataLocation(shader_prog, 0, "outColor");
    glLinkProgram(shader_prog);

    /* and check status of link */
    glGetProgramiv(shader_prog, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        glGetProgramiv(shader_prog, GL_INFO_LOG_LENGTH, &maxlength);
        if (maxlength > 0) {
            char buffer[maxlength];
            glGetProgramInfoLog(shader_prog, maxlength, NULL, buffer);
            console_set_error("OpenGL shader failed to link: ", buffer);
            console_destroy_gl(tty);
            return 1;
        }
    }

    glUseProgram(tty->shader_prog);

    /* Setup the buffer and attribute buffers so we can set values */
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    /* reserve size of buffer (one texture at a time) */
    glBufferData(GL_ARRAY_BUFFER, 
            sizeof(GLfloat) * 6 * 4, NULL, GL_DYNAMIC_DRAW);


    /* set the offset of the position in the buffer */
    posAttrib = glGetAttribLocation(shader_prog, "vertex");
    glVertexAttribPointer(posAttrib, 4, GL_FLOAT, GL_FALSE, 
            4 * sizeof(GLfloat), 0);

    glEnableVertexAttribArray(posAttrib);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    /* Set color for fragment shader */
    glUniform3f(glGetUniformLocation(shader_prog, "textColor"),
            1.0f, 1.0f, 1.0f);

    if (SDL_GL_SetSwapInterval(1) < 0)
        fprintf(stderr, "Warning: SwapInterval could not be set: %s\n", 
                SDL_GetError());

    return 0;
}

/*
 * Create a 1x1 texture which is used for its transparency value
 */
void
console_create_trans_texture (
        Console_tty *tty, 
        GLuint *texture,
        const unsigned char transparency)
{
    assert(tty);
    /* figure out the dimensions of the cursor and create pixel */
    unsigned char pixel[1] = { transparency };
    /* Generate the texture */
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RED, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* fill it with the pixel */
    glTexSubImage2D(
            GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, pixel);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void
console_on_new_input (Console_tty *tty, const char *input)
{
    assert(tty);
    assert(input);

    const auto str = from_utf8(input);
    /* if cursor is at end of line, it's a simple concatenation */
    if (tty->prompt.cursor == tty->prompt.input.length()) {
        tty->prompt.input += str;
    } else {
        /* else insert text into line at cursor's index */
        tty->prompt.input.insert(tty->prompt.cursor, str);
    }
    tty->prompt.cursor += str.length();
    tty->prompt.rebuild = true;
}

/*
 * Handle removing input with backspace or delete. We currenty only remove
 * input one character at a time, unlike inserting input.
 */

void
console_on_remove_input (Console_tty *tty)
{
    assert(tty);
    if (tty->prompt.cursor == 0 || tty->prompt.input.length() == 0)
        return;

    if (tty->prompt.input.length() == tty->prompt.cursor) {
        /* if cursor is at end of line just mark end of line at cursor */
        tty->prompt.input.pop_back();
    } else {
        /* else shift the text from cursor left by one character */
        tty->prompt.input.erase(tty->prompt.cursor, 1);
    }
    tty->prompt.cursor -= 1;
    tty->prompt.rebuild = true;
}

void
console_on_set_clipboard_text(Console_tty *tty)
{
    std::u32string ret;
    const std::u32string sep(U"\n");

    for (Console_Line *l = tty->lines_tail; l; l = l->prev) {
        if (l->selected) {
            if (!ret.empty()) ret += sep;

            if (false == l->text.empty())
                ret += l->text;
        }
    }
    SDL_SetClipboardText(to_utf8(ret).c_str());
}

int
console_on_key_down(Console_tty *tty, const SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_TAB:
        console_on_new_input_line(tty, from_utf8("(tab)"));
        break;

    case SDLK_BACKSPACE:
        console_on_remove_input(tty);
        break;

    case SDLK_RETURN:
        console_on_new_input_line(tty, tty->prompt.input);
        break;

    /* copy */
    case SDLK_c:
        if (SDL_GetModState() & KMOD_CTRL) {
            console_on_set_clipboard_text(tty);
        }
        break;

    /* paste */
    case SDLK_v:
        if (SDL_GetModState() & KMOD_CTRL) {
            char *str = SDL_GetClipboardText();
            if (*str != '\0')
                console_on_new_input(tty, str);
            SDL_free(str);
        }
        break;

    case SDLK_UP:
        console_update_prompt_from_history(tty, NavDirection::up);
        break;

    case SDLK_DOWN:
        console_update_prompt_from_history(tty, NavDirection::down);
        break;

    case SDLK_PAGEUP:
        console_on_scroll(tty, NavDirection::page_up);
        break;

    case SDLK_PAGEDOWN:
        console_on_scroll(tty, NavDirection::page_down);
        break;

    case SDLK_LEFT:
        if (tty->prompt.cursor > 0) {
            tty->prompt.cursor--;
            tty->prompt.rebuild = true;
        }
        break;

    case SDLK_RIGHT:
        if (tty->prompt.cursor < tty->prompt.input.length()) {
            tty->prompt.cursor++;
            tty->prompt.rebuild = true;
        }
        break;
    }
    return 0;
}

int
console_process_sdl_event (Console_tty *tty, SDL_Event *e)
{
    switch (e->type) {
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_RESIZED) {
            console_on_view_update(tty, ViewUpdate::resize);
        }
        break;
    case SDL_MOUSEWHEEL:
        if(e->wheel.y > 0) {
            console_on_scroll(tty, NavDirection::up);
        } else if(e->wheel.y < 0) {
            console_on_scroll(tty, NavDirection::down);
        }
        break;

    case SDL_MOUSEBUTTONDOWN:
        console_on_mouse_button_down(tty, e->button);
        break;

    case SDL_KEYDOWN:
        console_on_key_down(tty, e->key.keysym.sym);
        break;

    case SDL_TEXTINPUT:
        console_on_new_input(tty, e->text.text);
        break;
    }

    return 0;
}

int
console_sdl_event(void *data, SDL_Event *e)
{
    Console_tty *tty = static_cast<Console_tty *>(data);

    if (tty->status != State::claimed)
        return 1;
    /* XXX:
     * Might also need to look at mouse pos.
     * The window can remain unfocused even when
     * he cursor is hovering over the console
     * and overlapping another window.
     */
    const Uint32 flags = SDL_GetWindowFlags(tty->window);
    if (!(flags & SDL_WINDOW_INPUT_FOCUS)) {
        return 1;
    }

    /*
     * SDL_Event is a POD. String members are things
     * like char[32]. Should be safe to shallow copy.
     */
    SDL_Event ec;
    std::memcpy(&ec, e, sizeof(SDL_Event));
    tty->event_q.push_sdl(std::move(ec));
    return 0;
}

Console_tty*
Console_Create (const char *title,
                const char *prompt,
                const int font_size)

{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL Failed to init: %s\n", SDL_GetError());
    }

    SDL_Window *window = SDL_CreateWindow(title,
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            640, 480,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);

    if (!window) {
        fprintf(stderr, "Window could not be created: %s\n", SDL_GetError());
        return nullptr;
    }

    SDL_SetWindowMinimumSize(window, 48, 64);

    /*
     * SDL Console is meant to be used with OpenGL. Mix OpenGL and SDL draw
     * calls at your own peril.
     */
    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return nullptr;
    }

    /* XXX: Only supports one tty */
    Console_tty *tty = &tty_list[0];

    tty->gl_context   = glContext;
    tty->window       = window;
    tty->bg_color     = (Console_Color) { 0.0f, 0.0f, 0.0f, 0.9f };
    tty->font_color   = (Console_Color) { 1.0f, 1.0f, 1.0f, 1.0f };
    tty->render_thread_id = std::this_thread::get_id();

    if (console_init_ft(tty, font_size)) {
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        return nullptr;
    }

    if (console_init_gl(tty)) {
        console_destroy_ft(tty);
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        return nullptr;
    }

    tty->prompt.prompt_text = default_prompt;
    tty->prompt.h = tty->font.line_height;
    glGenTextures(1, &tty->prompt.texture);

    /* handle info that needs both freetype & opengl */
    console_create_trans_texture(tty, &tty->prompt.cursor_texture, 255 * 0.75f);
    console_create_trans_texture(tty, &tty->bg_texture, 255*tty->bg_color.a);
    /* kludgy, fill out tty->window_width, etc */
    console_on_view_update(tty, ViewUpdate::resize);

    SDL_SetEventFilter(console_sdl_event, tty);
    // XXX: is this needed?
    SDL_StartTextInput();

    tty->status = State::claimed;
    return tty;
}

void Console_SetPrompt(Console_tty *tty,
                       const char *prompt)
{
    auto str = from_utf8(prompt);
    tty->event_q.push_api([tty, str = std::move(str)] {
        tty->prompt.prompt_text = str;
        tty->prompt.rebuild = true;
    });
}

/* XXX: needs better return error codes and exit handling */
int
Console_Draw (Console_tty *tty)
{
    while (1) {
        /* if something's been written to the error string */
        if ( _Console_errstr[0] != '\0') {
            return -1;
          }

        if (console_render(tty))
            return -1;

        tty->event_q.wait_for_events();
        {
            std::scoped_lock lock(mutex);
            SDL_Event event;
            while (tty->event_q.pop_sdl(event)) {
                console_process_sdl_event(tty, &event);
            }
            ApiCall f;
            while(tty->event_q.pop_api(f)) {
                f();
            }
        }

        if (tty->status == State::shutdown) {
            break;
        }
    }
    return 0;
}

void
Console_AddLine (Console_tty *tty, const char *s)
{
    auto str = from_utf8(s);
    tty->event_q.push_api([tty, str = std::move(str)] {
        console_on_new_output_line(tty, str);
    });
}

/* XXX: needs queue if called by non-render thread
 * Set the background color of the console.
 * Default is 0.0f, 0.0f, 0.0f, 0.90f.
 */
void
Console_SetBackgroundColor (Console_tty *tty, const Console_Color c)
{
    std::scoped_lock lock(mutex);
    tty->bg_color = c;
    console_create_trans_texture(tty, &tty->bg_texture, 255*tty->bg_color.a);
}

void
Console_SetFontColor (Console_tty *tty, const Console_Color c)
{
    std::scoped_lock lock(mutex);
    tty->font_color = c;
}

int
Console_GetColumns (Console_tty *tty)
{
    std::scoped_lock lock(mutex);
    return (float)tty->window_width / (float)tty->font.char_width;
}

int
Console_GetRows (Console_tty *tty)
{
    std::scoped_lock lock(mutex);
    return (float)tty->window_height / (float)tty->font.line_height;
}

void
console_clear (Console_tty *tty)
{
    assert(tty);
    Console_Line *line;
    for (line = tty->lines_head; line != nullptr; line = line->next) {
        if (line->prev != nullptr) {
            glDeleteTextures(1, &line->prev->texture);
            delete line->prev;
        }
        if (line->next == nullptr) {
            glDeleteTextures(1, &line->texture);
            delete line;
            break;
        }
    }
    tty->lines_head = nullptr;
    tty->lines_tail = nullptr;
    tty->num_lines = 0;
}

void
Console_Clear (Console_tty *tty)
{
    tty->event_q.push_api([tty] { console_clear(tty); });
}

/*
 * Make sure to lock the mutex and stop the threads before destroying the
 * mutex and freeing any data the threads may be using.
 */
void
Console_Shutdown (Console_tty *tty)
{
    assert(tty);
    {
        std::scoped_lock lock(mutex);
        tty->status = State::shutdown;
        /* Jut put something in there so it isn't empty */
        tty->event_q.push_api([tty] { tty->status = State::shutdown; });
        tty->cv_input_completed = true;
    }

    tty->cv_input_completed.notify_one();
}

bool
Console_Destroy (Console_tty* tty)
{
    assert(tty);
    std::scoped_lock lock(mutex);
    if (std::this_thread::get_id() != tty->render_thread_id)
        return false;
    /*
     * There is no delete proc for SetEventFilter() in SDL2
     */
    SDL_SetEventFilter(nullptr, nullptr);
    SDL_QuitSubSystem(SDL_INIT_VIDEO); /* SDL keeps a ref count */
    console_destroy_ft(tty);
    console_destroy_gl(tty);
    console_clear(tty);
    SDL_GL_DeleteContext(tty->gl_context);
    SDL_DestroyWindow(tty->window);
    tty->status = State::unclaimed;
    return true;
}

int
Console_ReadLine(Console_tty *tty, std::string &buf)
{
    std::unique_lock<std::recursive_mutex> lock(mutex);
    if (tty->status != State::claimed)
        return 0;

    if (tty->input_complete_q.empty()) {
        lock.unlock();
        /*
         * atomic::wait. I am uncertain if vanilla condition
         * variables work well with recursive mutex
         */
        tty->cv_input_completed.wait(false);
        lock.lock();
        /*
         * May wake early from a shutdown event.
         */
        if (tty->input_complete_q.empty())
            return 0;
        tty->cv_input_completed = false;
    }

    buf = to_utf8(tty->input_complete_q.front());
    tty->input_complete_q.pop();
    return buf.length();
}

void
Console_SetMaxLines(Console_tty *tty, const size_t max_lines) {
    std::scoped_lock lock(mutex);
    tty->max_lines = max_lines;
}
