video-compare
=============

Split screen video comparison tool written in C++14 using FFmpeg libraries and SDL2. 

This tool can be used to visually compare e.g. the effect of codecs and resizing algorithms on
two video files played in sync. A movable slider enables easy viewing of the difference 
between two files across any region of interest.

Credits
-------

The code is mainly derived from the video player GitHub project: https://github.com/pockethook/player

Many thanks to the FFmpeg and SDL2 authors.

Usage
-----

    ./video-compare video1.mp4 video2.mp4

Controls
--------

* Space: Toggle play/pause
* Escape: Quit

Requirements
------------

Requires FFmpeg headers and development libraries to be installed, along with SDL2.

Build
-----

    make
