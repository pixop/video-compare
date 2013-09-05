#include "common.h"
#include "player.h"

using namespace std;

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
		exit(1);
	}

	return 0;
}
