#ifndef SRC_INTERFACE
#define SRC_INTERFACE

#include "display_geometry.h"
#include "renderer.h"
#include <string>

class Interface : public Renderer
{
public:
    Interface(SDL_Renderer *renderer, const DisplayGeometry &geometry);
    ~Interface();
    bool render(AVFrame *frame);
    bool drawHome(bool force, int state, const std::string &name);
    // Draw the toast/debug decorations on a cleared canvas and return the
    // height (in rows, from the top) they occupy -- 0 when nothing was drawn.
    // The DRM path commits the overlay plane over exactly that band, so the
    // live video below stays visible (needed on formats with no alpha).
    int drawOsd();
    void debug(const char *text);
    void showToast(const std::string &text);
    void hideToast();

private:
    int drawDebug();
    int drawToast();

    DisplayGeometry _geometry;
    int _state;
    bool _debug;
    bool _toast;
    RendererText _textStatus;
    RendererText _textDebug;
    RendererText _textToast;
    RendererImage _mainImage;
    std::string _debugText;
    std::string _toastText;
};

#endif /* SRC_INTERFACE */
