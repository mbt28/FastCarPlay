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
    bool drawHome(bool force, int state, std::string name);
    bool drawOsd();
    void debug(const char *text);
    void showToast(const std::string &text);
    void hideToast();

private:
    void drawDebug();
    void drawToast();

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
