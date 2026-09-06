#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include "fs.h"
#include "fs_error.h"

/*
 make filesystem from requested memory
 formats and closes fd
 separate mount/open path reopns for acutal use

 should check if file already exists by traversing

 returns -1 on failure
 */

void print_usage(const char *file_arg)
{
	fprintf(stderr,
			"usage: %s -f FILE [-k N | -m N | -g N]\n"
			"  -f FILE   backing file for the filesystem\n"
			"  -k N      size in KiB\n"
			"  -m N      size in MiB\n"
			"  -g N      size in GiB\n"
			"NOTE: No size specified will attempt to open a preexisting file\n"
			"WARNING: Redeclaring the size of an existing file will replace the existing file\n",
			file_arg);
}

std::vector<std::string> tokenize(std::string line)
{
	std::istringstream iss(line);

	std::vector<std::string> token_vec;
	std::string token;
	while (iss >> token) { token_vec.push_back(token); }
	return token_vec;
}

int main(int argc, char **argv)
{
	static_assert(sizeof(Inode_t) == (2 << 6));

	if (argc == 1) { print_usage(argv[0]); return 1; }

	const char *fs_name{nullptr};

	u64 fs_size{};

	int c;
	while ((c = getopt(argc, argv, "f:k:m:g:")) != -1)
	{
		switch(c)
		{
			case 'f':
				fs_name = optarg;
				break;
			case 'k':
				fs_size = KiB(strtoull(optarg, nullptr, 10));
				break;
			case 'm':
				fs_size = MiB(strtoull(optarg, nullptr, 10));
				break;
			case 'g':
				fs_size = GiB(strtoull(optarg, nullptr, 10));
				break;
			default:
				print_usage(argv[0]);
				exit(1);
		}
	}

	if (fs_name == nullptr)
	{
		fprintf(stderr, "ERROR: -f FILE is required\n");
		print_usage(argv[0]);
		return 1;
	}
	/*
	check the min file size, 
	if file size is 0, attempt to mount else create then mount
	*/
	bool create_fs = (fs_size != 0);

	if (create_fs) 
	{
		if (fs_size < KiB(256))
		{
			fprintf(stderr,
						"Reserved file system size should be atleast 256KiB\n");
			exit(2);
		}

		if (FS::mkfs(fs_name, fs_size) < 0)
		{
			fprintf(stderr, "Error creating filesystem");
			exit(3);
		}
	}
	// mount file system by calling constructor

	FS File_system = FS(fs_name);

	File_system.print_superblock();
	File_system.print_bitmaps();

	// loop on input, 
	std::string tokenize_prompt = fs_name;
	std::string line;

	while (File_system.is_running())
	{
		if (isatty(STDIN_FILENO)) std::cout << "\033[32m" << tokenize_prompt << ":" << File_system.get_curr_dir() <<   " " << "\033[0m"<< std::flush;
		if (!std::getline(std::cin, line)) { break; }

		auto args = tokenize(line);
		if (args.empty()) { break; }
		try
		{
			File_system.dispatch(args);
		}
		catch(const FSError& e)
		{
			fprintf(stderr, "%s\n", e.what());
		}
	}
	return 0;
}
