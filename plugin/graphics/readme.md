# Graphics Plugin

## Classes

### graphics_renderer

#### Methods

##### setup
Initializes the graphics system with a window handle.

`bool graphics_renderer::setup(uint64 window_handle);`

###### Arguments:
* uint64 window_handle: The handle to the window where graphics will be rendered.

###### Returns:
bool: true if initialization was successful, false otherwise.

###### Remarks:
This method must be called before any other graphics operations. You typically obtain the window handle from a game window object.

###### Example:
```NVGT
void main() {
    game_window win;
    graphics_renderer gfx;
    win.show();
    if (!gfx.setup(win.handle)) {
        alert("Error", "Failed to initialize graphics.");
        exit();
    }
}
```

##### load_image
Loads an image from a file.

`int graphics_renderer::load_image(const string &in filename);`

###### Arguments:
* const string &in filename: The path to the image file.

###### Returns:
int: The ID of the loaded image, or -1 if loading failed.

###### Remarks:
Supported formats generally include PNG, JPG, and BMP, depending on the underlying SDL_image configuration. The returned ID is used in `draw_image` calls.

##### draw_image
Draws a loaded image to the screen.

`void graphics_renderer::draw_image(int id, int x, int y, int w, int h);`

###### Arguments:
* int id: The ID of the image to draw (returned by `load_image`).
* int x: The x-coordinate of the top-left corner.
* int y: The y-coordinate of the top-left corner.
* int w: The width to draw the image.
* int h: The height to draw the image.

##### draw_image_region
Draws a specific region of a loaded image.

`void graphics_renderer::draw_image_region(int id, int x, int y, int w, int h, int sx, int sy, int sw, int sh);`

###### Arguments:
* int id: The ID of the image to draw.
* int x: The destination x-coordinate on the screen.
* int y: The destination y-coordinate on the screen.
* int w: The destination width.
* int h: The destination height.
* int sx: The source x-coordinate (region start) within the image.
* int sy: The source y-coordinate (region start) within the image.
* int sw: The source width (region width).
* int sh: The source height (region height).

###### Remarks:
This is useful for sprite sheets where multiple sprites are packed into a single image file.

##### load_font
Loads a font from a file and returns its ID.

`int graphics_renderer::load_font(const string &in font_name, float size, bool bold, bool italic);`

###### Arguments:
* const string &in font_name: The name or path of the font file. If just a name like "arial" is provided, the system font directory is usually searched.
* float size: The size of the font in points.
* bool bold: Whether to use bold style.
* bool italic: Whether to use italic style.

###### Returns:
int: The ID of the loaded font, or -1 if loading failed.

###### Remarks:
After loading a font, you must select it using `select_font` before drawing text.

##### select_font
Selects a previously loaded font for subsequent text rendering operations.

`bool graphics_renderer::select_font(int id);`

###### Arguments:
* int id: The ID of the font to select (returned by `load_font`).

###### Returns:
bool: true if the font was successfully selected, false if the ID was invalid.

###### Example:
```NVGT
// Load Arial, size 24, bold
int arial_id = gfx.load_font("arial", 24, true, false);
if (arial_id != -1) {
    gfx.select_font(arial_id);
}
```

##### draw_text
Draws text to the screen.

`void graphics_renderer::draw_text(const string &in text, int x, int y, int r, int g, int b);`

###### Arguments:
* const string &in text: The text to draw.
* int x: The x-coordinate.
* int y: The y-coordinate.
* int r: The red component of the text color (0-255).
* int g: The green component of the text color (0-255).
* int b: The blue component of the text color (0-255).

##### measure_text
Measures the dimensions of a text string.

`uint64 graphics_renderer::measure_text(const string &in text);`

###### Arguments:
* const string &in text: The text to measure.

###### Returns:
uint64: A packed 64-bit integer where the high 32 bits represent width and the low 32 bits represent height.

###### Remarks:
You can extract the width and height using bitwise operations:
```NVGT
uint64 size = gfx.measure_text("Hello");
int width = size >> 32;
int height = size & 0xFFFFFFFF;
```

##### draw_text_wrapped
Draws text wrapped within a specified width.

`void graphics_renderer::draw_text_wrapped(const string &in text, int x, int y, int w, int r, int g, int b);`

###### Arguments:
* const string &in text: The text to draw.
* int x: The x-coordinate.
* int y: The y-coordinate.
* int w: The maximum width for wrapping in pixels.
* int r: The red component (0-255).
* int g: The green component (0-255).
* int b: The blue component (0-255).

##### measure_text_wrapped
Measures the dimensions of a text string when wrapped.

`uint64 graphics_renderer::measure_text_wrapped(const string &in text, int w);`

###### Arguments:
* const string &in text: The text to measure.
* int w: The wrap width.

###### Returns:
uint64: A packed 64-bit integer (width << 32 | height).

##### clear_screen
Clears the screen with a specific color.

`void graphics_renderer::clear_screen(int r, int g, int b);`

###### Arguments:
* int r: Red component (0-255).
* int g: Green component (0-255).
* int b: Blue component (0-255).

##### draw_rect
Draws a rectangle.

`void graphics_renderer::draw_rect(int x, int y, int w, int h, int r, int g, int b, bool filled);`

###### Arguments:
* int x: X-coordinate.
* int y: Y-coordinate.
* int w: Width.
* int h: Height.
* int r: Red component.
* int g: Green component.
* int b: Blue component.
* bool filled: True to fill the rectangle, false for outline.

##### draw_circle
Draws a circle.

`void graphics_renderer::draw_circle(int x, int y, int radius, int r, int g, int b, bool filled);`

###### Arguments:
* int x: Center X-coordinate.
* int y: Center Y-coordinate.
* int radius: Radius of the circle.
* int r: Red component.
* int g: Green component.
* int b: Blue component.
* bool filled: True to fill the circle.

##### draw_line
Draws a line.

`void graphics_renderer::draw_line(int x1, int y1, int x2, int y2, int r, int g, int b, int thickness);`

###### Arguments:
* int x1: Start X.
* int y1: Start Y.
* int x2: End X.
* int y2: End Y.
* int r: Red component.
* int g: Green component.
* int b: Blue component.
* int thickness: Line thickness.

##### draw_menu
Draws a simple menu.

`void graphics_renderer::draw_menu(const string &in items, int selected_index, int x, int y);`

###### Arguments:
* const string &in items: A pipe-separated string of menu items (e.g., "Start Game|Options|Exit").
* int selected_index: The index of the currently selected item (0-based).
* int x: X-coordinate for the menu center.
* int y: Y-coordinate for the menu start.

###### Remarks:
The menu items must be separated by the `|` character. The selected item is automatically highlighted.

###### Example:
```NVGT
string menu_items = "New Game|Load Game|Quit";
int current_selection = 0;
// ... inside main loop ...
gfx.draw_menu(menu_items, current_selection, 400, 300);
```

##### present
Presents the rendered frame to the screen.

`void graphics_renderer::present();`

###### Remarks:
This must be called at the end of your rendering loop to actually display what has been drawn.