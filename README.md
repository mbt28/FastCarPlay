# FastCarPlay
A lightweight C++ CarPlay and Android Auto receiver for small embedded Linux
boards (primary target: Allwinner F1C200s). It speaks the native protocols
directly -- wireless/wired CarPlay and wireless/wired (AOAP) Android Auto --
with hardware H.264 decoding; no interposer dongle is involved.

![Logo](docs/images/screenshot.png)

## Setup

> [!IMPORTANT]  
> For detailed guide on set Raspberry Pi Zero 2W visit [Raspbery Pi Setup WIKI page](../../wiki/Raspbery-Pi-Setup)


### Dependencies
The project is based on SDL2, FFMPEG, LIBUSB (wired Android Auto / AOAP).
```
sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libusb-1.0-0-dev libssl-dev libasound2-dev libopus-dev
```

### USB permissions (wired Android Auto)
Wired Android Auto talks to the phone directly over USB (AOAP accessory mode),
which may need a udev rule granting access to the phone's USB device for your
user (match your phone's idVendor with `lsusb`).

### Build and run
The application can be started with settings file. Sample of the settings file can be found in settings.txt. Make sure that you put proper device id in settings and app have access to usb (see previous step)
The project is using make. You can edit ./conf/settings.txt after copy if needed. From the repository root run following
```
make clean
make release
mkdir ./conf
cp ./settings.txt ./conf
./out/app ./conf/settings.txt
```

### Customisation
You can change font and background images by replacing files in ./src/resource
- background.bmp for background image. Use BMP format only.
- font.ttf for font. Use TTF format only
- you can edit colours.h to modify text colours in RGBA format

The names of the file need to be exactly same. Resources are embedded in executable, remake the project to regenerate resources
```
make clean
make release
```

### Keys
The following keys have been mapped:
- Left - navigate left
- Right - navigate right
- Enter - select active item
- Backspace - Go back
- f - toggle fullscreen mode
- q - exit
- r - force refresh

## Status
What is working:
- Carplay and Android Auto
- Video
- Audio (multiple channels)
- Key navigation
- Simple touch
- Microphone (calls, siri)

What is not working:
- Multi touch - i have no means to test it and no idea how it should work
- Some android keys are not mapped
- No methods to switch between multiple paired wireless devices

### Notes
- Do not try to run debug builds if you do not need to debug application. They consume a lot of memory for address sanitising thats grow over time.
- Increasing FPS above Source-FPS will cause app to run UI loop with less delays and do more event polling. This can increase responsivenes of the system, but also will make X11 to use more resources.
- For multichannel audion (driving guidance over music) you need to have multichannel driver in the system (puslsaudio, pipewire). If you only have ALSA backend the second channel will not work
- Android has it's own video resolution system which is fixed for 480p 720p 1080p. So set up resolution that is closest to what you want and adjust DPI for UI scale. If you do not have full screen Android Auto, you might need to enable resolution negotiation. Go to Android Auto app info and search for "Additional settings in the app" option. Scrol down and tap fast 5 times on version. Now you can use top right three dots menu to go to "Developer settings". Tap "Video Resolution" and select "Allow to car and phone to negotiate".

### Progress and plans
Done
- [x] Implement direct buffer transfer from video decoder to renderer (should reduce amount of memory copies and CPU load)
- [x] Control audio buffers better (now system use 3 decoding threads but in reality only 2 required)
- [x] Reduce music volume when there is navigation messages
- [x] Add abilities to run script on device connect and device disconnect
- [x] Improve touch responsiveness
- [x] Android Auto (have not tested much, but seems to work for me)
- [x] Microphone support (Calls, Siri)

Next 
- [ ] Car menu with status, settings and options to run custom scripts
- [ ] Canbus script communication or some sort of side key press handling
- [ ] Better android navigation
- [ ] Switch between wireless devices

## Acknowledgement
The project is inspired and based on great work done by other developers:
- [pycarplay by electric-monk](https://github.com/electric-monk/pycarplay)
- [carplay-receiver by harrylepotter](https://github.com/harrylepotter/carplay-receiver)
- [react-carplay by rhysmorgan134](https://github.com/rhysmorgan134/react-carplay)
- [carplay-client by rayphee](https://github.com/rayphee/carplay-client)

The project is licenced under GPL-3 licence. See LICENCE for details.
The project is using [Open Sans font](https://fonts.google.com/specimen/Open+Sans). See FONT_LICENCE for details.

## Finally
If you have any questions, suggestions or you find problems running this feel free to open issue.
