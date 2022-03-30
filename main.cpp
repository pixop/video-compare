#define SDL_MAIN_HANDLED
#include "video_compare.h"
#include "argagg.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <regex>
#include <vector>

bool fileExists(const std::string &filename)
{
    std::ifstream fin(filename.c_str());
    return fin.is_open();
}

int main(int argc, char **argv)
{
    try
    {
        argagg::parser argparser{{{"help", {"-h", "--help"}, "show help", 0},
                                  {"high-dpi", {"-d", "--high-dpi"}, "allow high DPI mode for e.g. displaying UHD content on Retina displays", 0},
                                  {"window-size", {"-w", "--window-size"}, "override window size, specified as [width]x[height] (e.g. 800x600)", 1},
                                  {"time-shift", {"-t", "--time-shift"}, "shift the time stamps of the right video by a user-specified number of milliseconds", 1}}};

        argagg::parser_results args;
        args = argparser.parse(argc, argv);

        std::tuple<int, int> window_size(-1, -1);
        double time_shift_ms = 0;

        if (args["help"] || args.count() == 0)
        {
            std::ostringstream usage;
            usage
                << argv[0] << " 20220330-github" << std::endl
                << std::endl
                << "Usage: " << argv[0] << " [OPTIONS]... FILE1 FILE2" << std::endl
                << std::endl;
            argagg::fmt_ostream fmt(std::cerr);
            fmt << usage.str() << argparser;
        }
        else
        {
            if (args.pos.size() != 2)
            {
                throw std::logic_error{"Two FFmpeg compatible video files must be supplied"};
            }
            else
            {
                if (!fileExists(args.pos[0]))
                {
                    throw std::logic_error{"Unable to open the left video file"};
                }
                if (!fileExists(args.pos[1]))
                {
                    throw std::logic_error{"Unable to open the right video file"};
                }
            }
            if (args["window-size"])
            {
                const std::string window_size_arg = args["window-size"];
                const std::regex window_size_re("(\\d+)x(\\d+)");

                if (!std::regex_match(window_size_arg, window_size_re))
                {
                    throw std::logic_error{"Cannot parse window size argument (required format: [width]x[height], e.g. 800x600)"};
                }

                const std::regex delimiter_re("x");

                auto const token_vec = std::vector<std::string>(
                    std::sregex_token_iterator{begin(window_size_arg), end(window_size_arg), delimiter_re, -1},
                    std::sregex_token_iterator{});

                window_size = std::make_tuple(std::stoi(token_vec[0]), std::stoi(token_vec[1]));
            }
            if (args["time-shift"])
            {
                const std::string time_shift_arg = args["time-shift"];
                const std::regex time_shift_re("^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)$");

                if (!std::regex_match(time_shift_arg, time_shift_re))
                {
                    throw std::logic_error{"Cannot parse time shift argument; must be a valid floating point number"};
                }

                time_shift_ms = std::stod(time_shift_arg);
            }

            VideoCompare compare{args["high-dpi"], window_size, time_shift_ms, args.pos[0], args.pos[1]};
            compare();
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
