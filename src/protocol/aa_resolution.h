#ifndef SRC_PROTOCOL_AA_RESOLUTION
#define SRC_PROTOCOL_AA_RESOLUTION

// The pixel size behind an `aa-resolution` value.
//
// Android Auto cannot be asked for an arbitrary size: the video config in the
// service-discovery response carries a VideoCodecResolutionType *enum*, not a
// width and height, so only these three modes exist. (CarPlay is the opposite --
// it takes literal pixel dimensions, which is why it is asked for the display's
// exact size instead of a setting.)
//
// Both consumers need the pixels for their own reasons -- the native backend to
// define the touch coordinate space, the Carlinkit backend to fit the display's
// aspect inside the box -- so the table lives here rather than being copied.
inline void aa_resolution(int setting, int &width, int &height)
{
    switch (setting)
    {
    case 3:
        width = 1920;
        height = 1080;
        break;
    case 2:
        width = 1280;
        height = 720;
        break;
    default:
        width = 800;
        height = 480;
        break;
    }
}

#endif /* SRC_PROTOCOL_AA_RESOLUTION */
