#include <string>
#include <iostream>
#include <stdexcept>

#include "player.h"

using std::runtime_error;
using std::string;
using std::exception;
using std::cerr;
using std::endl;

int main(int argc, char **argv) {

	try {
		if (argc < 2) {
			throw runtime_error("Arguements");
		}

		string file_name = argv[1];

		Player player(file_name);

	}

	catch (exception &e) {
		cerr << "Initialization error: " << e.what() << endl;
		return 1;
	}

	return 0;
}
