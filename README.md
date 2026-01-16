# Videoplayer

Written entirely in C using SDL3. Can read a video/audio stream with a set resolution
Made to watch movies on my video capturing device.

usage: v4l2_sdl_view [width] [height] [video] [audio]
Without specifying parameters the program will always launch with the following defaults:
640x480 video=/dev/video0 audio="USB3. 0 capture Stereo analogico"
