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

#define INT_CEIL_DIV(a, b) (((a) + (b) - 1) / (b))

constexpr u64 KiB(u64 n) { return n << 10; }
constexpr u64 MiB(u64 n) { return n << 20; }
constexpr u64 GiB(u64 n) { return n << 30; }

constexpr u64 block_size{KiB(4)};

/*
 make filesystem from requested memory
 formats and closes fd
 separate mount/open path reopns for acutal use

 should check if file already exists by traversing

 returns -1 on failure
 */

int mkfs(const char* pathname, u64 sz)
{
	int fd = open(pathname, O_CREAT | O_EXCL | O_RDWR, 0644);

	if (fd < 0)
	{
		if (errno == EEXIST)
		{
			perror("mkfs: file system already exists: ");
		}
		else
		{
			perror("mkfs: failed to open file system");
		}
		return -1;
	}

	if (ftruncate(fd, sz) < 0)
	{
		perror("Failed to truncate to specified size");
		close(fd);
		return -1;
	}
	// 
	u32 block_count = sz / block_size;
	u32 inode_count = INT_CEIL_DIV(block_count, 8); // 1 inode every 8 blocks
	
	// inode bitmap offset
	u32 inode_bitmap_offset = 1;
	u32 inode_bitmap_sz = INT_CEIL_DIV(inode_count, 8);
	u32 inode_bitmap_blocks = INT_CEIL_DIV(inode_bitmap_sz, block_size);
	
	// data bitmap offset
	u32 data_bitmap_offset = inode_bitmap_offset + inode_bitmap_blocks;
	u32 data_bitmap_sz = INT_CEIL_DIV(block_count, 8);
	u32 data_bitmap_blocks = INT_CEIL_DIV(data_bitmap_sz, block_size);

	// inode table offset
	u32 inode_table_offset = data_bitmap_offset + data_bitmap_blocks;
	u64 inode_table_sz = (u64)inode_count * sizeof(inode_t);
	u32 inode_table_blocks = INT_CEIL_DIV(inode_table_sz, block_size);

	// data region offfset
	u32 data_region_offset = inode_table_offset + inode_table_blocks;

	Bitmap inode_bitmap = Bitmap(inode_count);
	Bitmap data_bitmap = Bitmap(block_count);

	super_block_t sb =
	{
		.sz = sz,
		.magic_number= MAGIC_NUMBER, 
		.block_size = block_size, 
		.inode_count = inode_count,
		.inode_bitmap_offs = inode_bitmap_offset,
		.data_bitmap_offs = data_bitmap_offset,
		.inode_table_offs = inode_table_offset,
		.data_region_offs = data_region_offset,
	};

	// mark superblock and inode table blocks as used (ALL METADATA and root directory entries)
	for (u32 i = 0; i < data_region_offset; ++i) { data_bitmap.set_bit(i); }
	data_bitmap.set_bit(sb.data_region_offs);

	// write superblock to file

	if (pwrite(fd, &sb, sizeof(sb), 0) != (ssize_t)sizeof(sb))
	{
		perror("Failed to write superblock");
		close(fd);
		return -1;
	}

	// create root

	// write directory entries
	const entry_t root_dir_entries[] = {
		{ .inode_num = 0, .name = "."},
		{ .inode_num = 0, .name = ".."},
	};

	if (pwrite(fd, root_dir_entries, sizeof(root_dir_entries), sb.data_region_offs * block_size) != (ssize_t)sizeof(root_dir_entries))
	{
		perror("Failed to write root directory entry");
		close(fd);
		return -1;
	}

	// mark inode 0 ase used (root)
	inode_bitmap.set_bit(0);

	// write root inode
	u32 root_sz = sizeof(root_dir_entries);
	u32 root_blocks = INT_CEIL_DIV(root_sz, block_size);
	inode_t root =
	{
		.size = root_sz,
		.time = 0,
		.ctime = 0,
		.mtime = 0,
		.dtime = 0,
		.blocks = root_blocks,
		.flags = {},
		.osd1 = 0,
		.block_ptrs = { sb.data_region_offs },
		.mode = 0,
		.uid = 0,
		.gid = 0,
		.links_count = 2,
		.type = 1,
		.padding = {},
	};

	u32 root_offs = 0;
	// write inode to table and directory entry to data
	if (pwrite(fd, &root, sizeof(root), (sb.inode_table_offs + root_offs) * block_size) != (ssize_t)sizeof(root))
	{
		perror("Failed to write root");
		close(fd);
		return -1;
	}
	
	// write bitmaps
	std::vector<u8> inode_bitmap_vec = inode_bitmap.get_bytes();
	std::vector<u8> data_bitmap_vec = data_bitmap.get_bytes();
	// inode bitmap

	if (pwrite(fd, inode_bitmap_vec.data(), inode_bitmap_vec.size(), sb.inode_bitmap_offs * block_size) != (ssize_t)inode_bitmap_vec.size())
	{
		perror("Failed to write inode bitmap");
		close(fd);
		return -1;
	}

	if (pwrite(fd, data_bitmap_vec.data(), data_bitmap_vec.size(), sb.data_bitmap_offs * block_size) != (ssize_t)data_bitmap_vec.size())
	{
		perror("Failed to write data bitmap");
		close(fd);
		return -1;
	}

	if (fsync(fd) < 0)
	{
		perror("Failed to sync filesystem to disk");
		close(fd);
		return -1;
	}
	
	close(fd);
	return 0;
}


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

	static_assert(sizeof(inode_t) == (2 << 6));

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

		if (mkfs(fs_name, fs_size) < 0)
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

	while (true)
	{
		if (isatty(STDIN_FILENO)) std::cout << "\033[32m" << tokenize_prompt << ":" << File_system.get_curr_dir() <<   " " << "\033[0m"<< std::flush;
		if (!std::getline(std::cin, line)) { break; }

		auto args = tokenize(line);
		if (args.empty()) { break; }

		std::string cmd = args[0];
		if ((cmd == "exit") || (cmd == "quit") || (cmd == "q")) { break; }
		else if (cmd == "mkdir")
		{ 
			if (args.size() != 2) { continue; }
			std::string path= args[1];
			File_system.mkdir(path);
		}
		else if (cmd == "ls")
		{ 
			// handle "ls" itsself
			std::string path = "";
			if (args.size() == 2) { path = args[1]; }
			File_system.ls(path);
		}
		else if (cmd == "cd")
		{
			std::string path = "";
			if (args.size() == 2) { path = args[1]; }
			File_system.cd(path);
		}
		else if (cmd == "touch")
		{ 
			if (args.size() != 2) { continue; }
			std::string path = args[1];
			File_system.touch(path);
		}
		else if (cmd == "rm")
		{
			std::string path = "";
			if (args.size() == 2) { path = args[1]; }
			File_system.rm(path);
		}
		else if (cmd == "write")
		{
			std::string path = "";
			if (args.size() <= 2) { continue; }
			path = args[1];
			std::string buf = "";
			for (size_t i{2}; i < args.size(); ++i)
			{
				if (i > 2) buf += ' ';
				buf += args[i];
			}
			File_system.write(path, buf);
		}
		else if (cmd == "cat")
		{
			std::string path = "";
			if (args.size() == 2) { path = args[1]; }
			File_system.cat(path);
		}
	}
	return 0;
}
