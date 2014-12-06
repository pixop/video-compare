#include <iostream>
#include <stdexcept>
#include <string>

#include "player.h"

using std::cerr;
using std::endl;
using std::exception;
using std::logic_error;

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
