#ifndef SDL_CONSOLE
#define SDL_CONSOLE

#include <cstddef>
#include <string>
#include <SDL2/SDL.h>

typedef struct SDL_Window SDL_Window;

struct _SDL_console_tty;
typedef struct _SDL_console_tty Console_tty;

typedef struct _console_color {
    float r, g, b, a;
} Console_Color;

/*
 * Create the console. 
 * The console will load the font at `font_path'. The font path *must* be a
 * monospaced or fixed-width. 
 * The `trigger_key' is the keyboard key that toggles the console on and off.
 * `input_func' is the function that uses the input to the console and 
 * `input_func_data' is userdata given to that function.
 * Returns NULL on error.
 */

/*
Console_tty* 
Console_Create (SDL_Window *window, 
                const char *font_path, 
                const int font_size,
                SDL_Keycode trigger_key,
                Console_InputFunction input_func,
                void *input_func_data);
		*/

extern "C" {
Console_tty* 
Console_Create (const char *title,
                const char *prompt,
                const int font_size);

void
Console_Shutdown (Console_tty *tty);

/*
 * Clean up the console.
 */
bool
Console_Destroy (Console_tty* tty);

/*
 * Set the background color of the console.
 * Default is 0.0f, 0.0f, 0.0f, 0.90f.
 */
void
Console_SetBackgroundColor (Console_tty *tty, Console_Color);

/*
 * Set the font color.
 * Default is 1.0f, 1.0f, 1.0f, 1.0f.
 */
void
Console_SetFontColor (Console_tty *tty, Console_Color);

int
Console_GetColumns (Console_tty *tty);

int
Console_GetRows (Console_tty *tty);

void
Console_Clear (Console_tty *tty);

void
Console_SetPrompt(Console_tty *tty, const char *prompt);

int
Console_Draw (Console_tty *tty);

/*
 * Get the last error.
 */
const char *
Console_GetError (void);

void
Console_AddLine (Console_tty *tty, const char *s);

int
Console_ReadLine(Console_tty *tty, std::string &buf);

bool
Console_HasFocus(Console_tty *tty);

void
Console_SetMaxLines(Console_tty *tty, size_t max_lines);

/*
int
Console_InputWatch (Console_tty *tty, void *e);*/

}


#endif
