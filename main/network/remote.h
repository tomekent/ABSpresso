#pragma once

// Remote control over the home network: a small web page (Now Playing and the library) and the
// JSON API behind it, served on port 80 and advertised as http://abspresso.local.
//
//   GET  /                    the control page
//   GET  /api/status          what's playing, position, chapter, volume, sleep timer, the server's
//                             address
//   GET  /api/books           the library shown on the device (id, title, author, progress,
//                             download state)
//   GET  /api/cover?id=       a cached cover JPEG (SD card only; 404 otherwise). The page prefers
//                             the Audiobookshelf server's cover URL and falls back to this.
//   POST /api/toggle          play / pause (with nothing loaded, resumes the latest book)
//   POST /api/play?id=        play a book from the library
//   POST /api/skip?dir=-1|1   skip back / forward by the device's skip lengths
//   POST /api/seek?to=s       jump to a position in the book (seconds)
//   POST /api/chapter?d=-1|1  previous / next chapter
//   POST /api/volume?v=0-100
//   POST /api/stop
//   POST /api/sleep?min=      sleep timer: minutes of playback, -1 for the end of the chapter, 0 off
//   GET  /api/settings        brightness, screen-off and sleep timers, skip lengths, rotation,
//                             the server's libraries and the selected one
//   POST /api/settings?brightness=&screen_off_s=&sleep_min=&skip_back_s=&skip_fwd_s=&rotate180=
//                             any subset; applied and saved as the device's Settings page does
//   POST /api/library?id=     switch library (the device reloads it)
//   GET  /api/downloads       SD card space, and each book that's queued, downloading or saved
//   POST /api/download?id=    download a book to the SD card (books only, not podcasts)
//   POST /api/download?id=&remove=1   cancel a download or delete the saved copy
//
// Not running while the setup portal is (it owns port 80 then).

void remote_start(void);
void remote_stop(void);
