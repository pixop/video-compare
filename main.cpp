#define SDL_MAIN_HANDLED
#include "video_compare.h"
#include "argagg.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>

int main(int argc, char** argv) {
	try {
        argagg::parser argparser {{
            { "help", {"-h", "--help"},
            "show help", 0},
            { "high-dpi", {"-d", "--high-dpi"},
            "allow high DPI mode for e.g. displaying UHD on Retina displays", 0}
        }};

        argagg::parser_results args;
        args = argparser.parse(argc, argv);

        if (args["help"] || args.count() == 0) {
            std::ostringstream usage;
            usage
                << argv[0] << " 0.10-beta" << std::endl
                << std::endl
                << "Usage: " << argv[0] << " [OPTIONS]... FILE1 FILE2" << std::endl
                << std::endl;
            argagg::fmt_ostream fmt(std::cerr);
            fmt << usage.str() << argparser;
        } else {
            if (args.pos.size() != 2) {
                throw std::logic_error{"Two FFmpeg compatible video files must be supplied"};
            }

            VideoCompare compare{args["high-dpi"], args.pos[0], args.pos[1]};
            compare();
        }
	}
	catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return -1;
	}

	return 0;
}
