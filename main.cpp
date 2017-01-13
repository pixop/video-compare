#include "player.h"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
	try {
		if (argc != 2) {
			throw std::logic_error{"Not enough arguments"};
		}

		Player play{argv[1]};
		play();
	}

	catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return -1;
	}

	return 0;
}
