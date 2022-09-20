#define SDL_MAIN_HANDLED
#include "video_compare.h"
#include "argagg.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <regex>
#include <vector>

#ifdef _WIN32
    #include <Windows.h>

    // Credits to Mircea Neacsu, https://github.com/neacsum/utf8
    char** get_argv (int* argc, char **argv)
    {
        char** uargv = nullptr;
        wchar_t** wargv = CommandLineToArgvW (GetCommandLineW (), argc);
        if (wargv)
        {
            uargv = new char* [*argc];
            for (int i = 0; i < *argc; i++)
            {
                int nc = WideCharToMultiByte (CP_UTF8, 0, wargv[i], -1, 0, 0, 0, 0);
                uargv[i] = new char[nc + 1];
                WideCharToMultiByte (CP_UTF8, 0, wargv[i], -1, uargv[i], nc, 0, 0);
            }
            LocalFree (wargv);
        }
        return uargv;
    }

    void free_argv (int argc, char** argv)
    {
        for (int i = 0; i < argc; i++)
        {
            delete argv[i];
        }
        delete argv;
    }
#else
    #define UNUSED(x) (void)(x)

    char** get_argv (int* argc, char **argv)
    {
        UNUSED(argc);
        return argv;
    }

    void free_argv (int argc, char** argv)
    {
        UNUSED(argc);
        UNUSED(argv);
    }
#endif

int main(int argc, char **argv)
{
    int exit_code = 0;

    char **argv_decoded = get_argv(&argc, argv);

    try
    {
        argagg::parser argparser{{{"help", {"-h", "--help"}, "show help", 0},
                                  {"high-dpi", {"-d", "--high-dpi"}, "allow high DPI mode for e.g. displaying UHD content on Retina displays", 0},
                                  {"display-mode", {"-m", "--mode"}, "display mode (layout), 'split' for split screen (default), 'vstack' for vertical stack, 'hstack' for horizontal stack", 1},
                                  {"window-size", {"-w", "--window-size"}, "override window size, specified as [width]x[height] (e.g. 800x600)", 1},
                                  {"time-shift", {"-t", "--time-shift"}, "shift the time stamps of the right video by a user-specified number of milliseconds", 1}}};

        argagg::parser_results args;
        args = argparser.parse(argc, argv_decoded);

        std::tuple<int, int> window_size(-1, -1);
        double time_shift_ms = 0;
        Display::Mode display_mode = Display::Mode::split;

        if (args["help"] || args.count() == 0)
        {
            std::ostringstream usage;
            usage
                << argv[0] << " 20220918-github" << std::endl
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
            if (args["display-mode"])
            {
                const std::string display_mode_arg = args["display-mode"];

                if (display_mode_arg == "split")
                {
                    display_mode = Display::Mode::split;
                }
                else if (display_mode_arg == "vstack")
                {
                    display_mode = Display::Mode::vstack;
                }
                else if (display_mode_arg == "hstack")
                {
                    display_mode = Display::Mode::hstack;
                }
                else
                {
                    throw std::logic_error{"Cannot parse display mode argument (valid options: split, vstack, hstack)"};
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

            VideoCompare compare{display_mode, args["high-dpi"], window_size, time_shift_ms, args.pos[0], args.pos[1]};
            compare();
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        exit_code = -1;
    }

    free_argv(argc, argv_decoded);

    return exit_code;
}
