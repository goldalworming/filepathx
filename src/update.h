#ifndef UPDATE_H
#define UPDATE_H

#include <windows.h>

/* Auto-update against GitHub Releases (goldalworming/filepathx).
   Check runs on a worker thread; install downloads the release zip and
   hands over to a small PowerShell script that swaps the exe once the
   process exits, then relaunches it. */

enum {
    UPDATE_IDLE = 0,         /* no check performed yet          */
    UPDATE_CHECKING,
    UPDATE_UP_TO_DATE,
    UPDATE_AVAILABLE,        /* update_latest_version() has the tag */
    UPDATE_DOWNLOADING,
    UPDATE_RESTART_PENDING,  /* updater launched; app should quit   */
    UPDATE_ERROR             /* update_error() has the reason       */
};

/* notify_msg is PostMessage'd to hwnd whenever the status changes.
   state_file (UTF-8 path) persists the 24 h check throttle. */
void update_init(HWND hwnd, UINT notify_msg, const char* state_file);

/* force=0 respects the 24 h throttle (may answer from cache without
   touching the network); force=1 always hits the GitHub API. */
void update_check_async(int force);

/* Download the latest release and launch the swap-and-restart script.
   Only valid while status is UPDATE_AVAILABLE. */
void update_install_async(void);

int         update_status(void);
void        update_latest_version(char* out, int n);   /* e.g. "v0.8.0" */
const char* update_error(void);

#endif
