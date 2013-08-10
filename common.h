#pragma once

extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavutil/avutil.h>
}

#include <SDL.h>
#include <SDL_thread.h>

#include <iostream>
#include <cstring>
#include <algorithm>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>

#include "queue.h"
