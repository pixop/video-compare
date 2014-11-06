#include <string>
#include <iostream>
#include <stdexcept>

#include "player.h"

using std::logic_error;
using std::exception;
using std::cerr;
using std::endl;

int main(int argc, char** argv) {
	try {
		if (argc != 2) {
			throw logic_error{"Not enough arguments"};
		}

		Player player{argv[1]};
	}

	catch (exception &e) {
		cerr << "Error: " << e.what() << endl;
		return -1;
	}

	return 0;
}
