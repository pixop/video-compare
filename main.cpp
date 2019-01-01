#include "video_compare.h"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
	try {
		if (argc != 3) {
			throw std::logic_error{"Two arguments to FFmpeg compatible video files must be supplied"};
		}

		VideoCompare compare{argv[1], argv[2]};
		compare();
	}

	catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return -1;
	}

	return 0;
}
