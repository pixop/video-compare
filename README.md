video-compare
=============

Split screen video comparison tool written in C++14 using FFmpeg libraries and SDL2. 

`video-compare` can be used to visually compare e.g. the effect of codecs and resizing algorithms on
two video files played in sync. The tool is not very restrictive as videos are not required to be the
same resolution, color format, container format, codec or duration. However, for the best result video
files should have the same frame rate.

A movable slider enables easy viewing of the difference across any region of interest.

Thanks to the versatility of FFmpeg, it is actually also possible to use `video-compare` to compare
two images. The common PNG and JPEG formats have been successfully tested to work.

![Screenshot](screenshot_1.jpg?raw=true)
![Screenshot](screenshot_2.jpg?raw=true)

Credits
-------

`video-compare` was created by Jon Frydensbjerg (email: jon@pixop.com). The code is mainly based on
the excellent video player GitHub project: https://github.com/pockethook/player

Many thanks to the FFmpeg and SDL2 authors.

Usage
-----

Launch in disallow high DPI mode. Video pixels become doubled on high DPI displays. Recommended
for displaying HD 1080p video on e.g. a Retina 5K display:

    ./video-compare video1.mp4 video2.mp4

Allow high DPI mode on systems which supports that. Video pixels are displayed "1-to-1". Useful
for e.g. displaying UHD 4K video on a Retina 5K display:

    ./video-compare -d video1.mp4 video2.mp4

Controls
--------

* Space: Toggle play/pause
* Escape: Quit
* Down arrow: Seek 15 seconds backward
* Left arrow: Seek 1 second backward
* Page down: Seek 600 seconds backward
* Up arrow: Seek 15 seconds forward
* Right arrow: Seek 1 second forward
* Page up: Seek 600 seconds forward
* S: Swap left and right video
* A: Previous frame
* D: Next frame
* Z: Zoom area around cursor (result shown in lower left corner)
* C: Zoom area around cursor (result shown in lower right corner)
* 1: Toggle hide/show left video
* 2: Toggle hide/show right video
* 3: Toggle hide/show HUD
* 0: Toggle video/subtraction mode

Move the mouse horizontally to adjust the movable slider position.

Requirements
------------

Requires FFmpeg headers and development libraries to be installed, along with SDL2
and its TrueType font rendering add on (libsdl2_ttf).

Build
-----

    make

Notes
-----

1. Audio playback is not supported.

2. The code is hard to maintain at the moment (too many copy/paste and lazy solutions).
My intention has mainly been to build a tool in a few days which gets the job done. 
A lot of refactoring and clean-up is required when I have the time for it. Consider
yourself warned! ;-)
