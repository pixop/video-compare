#pragma once

extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavutil/avutil.h>
	#include <libavutil/time.h>
}

#include <SDL.h>
#include <SDL_thread.h>

#include <string>
#include <cstring>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <cmath>

#include "queue.h"
