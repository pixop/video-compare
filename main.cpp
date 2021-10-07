#define SDL_MAIN_HANDLED
#include "video_compare.h"
#include "argagg.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <regex>
#include <vector>

int main(int argc, char **argv)
{
    try
    {
        argagg::parser argparser{{{"help", {"-h", "--help"}, "show help", 0},
                                  {"high-dpi", {"-d", "--high-dpi"}, "allow high DPI mode for e.g. displaying UHD content on Retina displays", 0},
                                  {"window-size", {"-w", "--window-size"}, "override window size specified as [width]x[height] (e.g. 800x600)", 1}}};

        argagg::parser_results args;
        args = argparser.parse(argc, argv);

        std::tuple<int, int> window_size(-1, -1);

        if (args["help"] || args.count() == 0)
        {
            std::ostringstream usage;
            usage
                << argv[0] << " 0.11-beta" << std::endl
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
            if (args["window-size"])
            {
                const std::string str = args["window-size"];
                const std::regex window_size_re("(\\d+)x(\\d+)");

                if (!std::regex_match(str, window_size_re))
                {
                    throw std::logic_error{"Cannot parse window size (required format: [width]x[height], e.g. 800x600)"};
                }

                const std::regex delimiter("x");

                auto const vec = std::vector<std::string>(
                    std::sregex_token_iterator{begin(str), end(str), delimiter, -1},
                    std::sregex_token_iterator{});

                std::cout << "Hello"
                          << " " << str << " " << vec[0] << std::endl;

                window_size = std::make_tuple(std::stoi(vec[0]), std::stoi(vec[1]));
            }

            VideoCompare compare{args["high-dpi"], window_size, args.pos[0], args.pos[1]};
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
