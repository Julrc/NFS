#include "fs.h"


#include <cstring>

//  TODO: 
//  OFFSET OVERFLOWS WITH u32
//  Links_count
//  throwing as error handling


int FS::mkfs(const char* pathname, u64 sz)
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
	u32 block_count = sz / BLOCK_SIZE;
	u32 inode_count = INT_CEIL_DIV(block_count, 8); // 1 inode every 8 blocks
	
	// inode bitmap offset
	u32 inode_bitmap_offset = 1;
	u32 inode_bitmap_sz = INT_CEIL_DIV(inode_count, 8);
	u32 inode_bitmap_blocks = INT_CEIL_DIV(inode_bitmap_sz, BLOCK_SIZE);
	
	// data bitmap offset
	u32 data_bitmap_offset = inode_bitmap_offset + inode_bitmap_blocks;
	u32 data_bitmap_sz = INT_CEIL_DIV(block_count, 8);
	u32 data_bitmap_blocks = INT_CEIL_DIV(data_bitmap_sz, BLOCK_SIZE);

	// inode table offset
	u32 inode_table_offset = data_bitmap_offset + data_bitmap_blocks;
	u64 inode_table_sz = (u64)inode_count * sizeof(Inode_t);
	u32 inode_table_blocks = INT_CEIL_DIV(inode_table_sz, BLOCK_SIZE);

	// data region offfset
	u32 data_region_offset = inode_table_offset + inode_table_blocks;

	Bitmap inode_bitmap = Bitmap(inode_count);
	Bitmap data_bitmap = Bitmap(block_count);

	Super_block_t sb =
	{
		.sz = sz,
		.magic_number= MAGIC_NUMBER, 
		.block_size = BLOCK_SIZE, 
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
	const Entry_t root_dir_entries[] = {
		{ .inode_num = 0, .name = "."},
		{ .inode_num = 0, .name = ".."},
	};

	if (pwrite(fd, root_dir_entries, sizeof(root_dir_entries), sb.data_region_offs * BLOCK_SIZE) != (ssize_t)sizeof(root_dir_entries))
	{
		perror("Failed to write root directory entry");
		close(fd);
		return -1;
	}

	// mark inode 0 ase used (root)
	inode_bitmap.set_bit(0);

	// write root inode
	u32 root_sz = sizeof(root_dir_entries);
	u32 root_blocks = INT_CEIL_DIV(root_sz, BLOCK_SIZE);
	Inode_t root =
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
	if (pwrite(fd, &root, sizeof(root), (sb.inode_table_offs + root_offs) * BLOCK_SIZE) != (ssize_t)sizeof(root))
	{
		perror("Failed to write root");
		close(fd);
		return -1;
	}
	// write bitmaps
	std::vector<u8> inode_bitmap_vec = inode_bitmap.get_bytes();
	std::vector<u8> data_bitmap_vec = data_bitmap.get_bytes();
	// inode bitmap

	if (pwrite(fd, inode_bitmap_vec.data(), inode_bitmap_vec.size(), sb.inode_bitmap_offs * BLOCK_SIZE) != (ssize_t)inode_bitmap_vec.size())
	{
		perror("Failed to write inode bitmap");
		close(fd);
		return -1;
	}

	if (pwrite(fd, data_bitmap_vec.data(), data_bitmap_vec.size(), sb.data_bitmap_offs * BLOCK_SIZE) != (ssize_t)data_bitmap_vec.size())
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

FS::FS(std::string name) : m_name{ std::move(name) }, m_cwd_inode{ 0 }
{
	// read superblock (mount logic, resource acquisition)
	int fd = open(m_name.c_str(), O_RDWR);
	if (fd < 0) { perror("openfs: open"); return; }
	m_fd = fd;

	//read superblock and verify magic number
	ssize_t bytes_read_sb = pread(fd, &sb, sizeof(sb), 0);
	if (bytes_read_sb < 0 || (size_t)bytes_read_sb != sizeof(sb))
	{
		fprintf(stderr, "Error reading superblock\n");
		return;
	}

	if (sb.magic_number != MAGIC_NUMBER)
	{
		fprintf(stderr, "FS does not match: bad magic number\n");
		return;
	}

	u32 inode_bitmap_sz = (sb.inode_count + 7) / 8;
	std::vector<u8> inode_bitmap_vec(inode_bitmap_sz);
	ssize_t bytes_read_ib = pread(fd, inode_bitmap_vec.data(), inode_bitmap_vec.size(), sb.inode_bitmap_offs * sb.block_size);
	if (bytes_read_ib < 0 || ((size_t)bytes_read_ib != inode_bitmap_vec.size()))
	{
		fprintf(stderr, "Error reading inode bitmap\n");
		return;
	}

	inode_bitmap.set_vector(inode_bitmap_vec, sb.inode_count);

	u32 fs_block_count = sb.sz / sb.block_size;
	u32 data_bitmap_sz = (fs_block_count + 7) / 8;
	std::vector<u8> data_bitmap_vec(data_bitmap_sz);

	ssize_t bytes_read_db = pread(fd, data_bitmap_vec.data(), data_bitmap_vec.size(), sb.data_bitmap_offs * sb.block_size);
	if (bytes_read_db < 0 || ((size_t)bytes_read_db != data_bitmap_vec.size()))
	{
		fprintf(stderr, "Error reading data bitmap\n");
		return;
	}
	data_bitmap.set_vector(data_bitmap_vec, fs_block_count);
}

FS::~FS()
{
	if (m_fd >= 0) { close(m_fd); }
}


void FS::mkdir(std::string raw_path)
{
	// parse, directories 

	auto path = resolve_path(raw_path);
	if (!path) { fprintf(stderr, "invalid path\n"); return; }

	std::string name = path->name;
	u32 target_parent = path->parent;

	if (find_in_dir(target_parent, name)) { fprintf(stderr, "mkdir: directory already exists\n"); return; }
	if (!create_dir(target_parent, name)) { fprintf(stderr, "mkdir: error creating directory\n"); return; }

	sync();
	// ex. /root/etc
}

void FS::ls(std::string raw_path)
{

	auto path = resolve_path(raw_path);
	if (!path) { fprintf(stderr, "invalid path\n"); return; }

	auto target = path->target; // default for root
	if (!target) { fprintf(stderr, "failed to find target\n"); return; }

	// out put target directories
	
	auto meta = read_inode_meta(target.value());
	if (!meta) { fprintf(stderr, "ls: unable to read directory metadata\n"); return; }
	if (!is_dir(meta.value())) { fprintf(stderr, "invalid, not a directory\n"); }
	auto data = read_inode_data(meta);
	if (!data) { fprintf(stderr, "ls: unable to read directory data\n"); return;}

	std::vector<Entry_t> directory_entries = data_parse_dirs(data.value());

	for (auto &entry : directory_entries) { printf("%u, %s\n", entry.inode_num, entry.name); }
}

// from current directory m_curr_dir, traverse to path
// later on save curr dir inode num to remove need to traverse from root
void FS::cd(std::string raw_path)
{
	Path_req_t path = parse_path(raw_path);
	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "invalid path\n"); return; }
	if (!res_path->target) { fprintf(stderr, "no such directory\n"); return; }
	if (!is_dir(res_path->target.value())) { fprintf(stderr, "not a directory\n"); return; };

	// you either cd from an absolute path,  or relative
	
	std::vector<std::string> curr_parts = (path.absolute) ? std::vector<std::string>{}: m_cwd_parts;
	for (auto &p : path.path_parts)
	{
		if (p == "..") { if (!curr_parts.empty()) curr_parts.pop_back(); }
		else { curr_parts.push_back(p); }
	}

	m_cwd_parts = std::move(curr_parts);
	m_cwd_inode = res_path->target.value();
}

void FS::rm(std::string raw_path)
{
	Path_req_t path = parse_path(raw_path);
	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "no such directory\n"); return; }

	// refuse if deleting a descendant of current directory
	bool is_descendant{false};
	if (path.absolute)
	{
		if ((path.path_parts.size() < m_cwd_parts.size()) && (std::equal(path.path_parts.begin(), path.path_parts.end(), m_cwd_parts.begin())))
		{
			is_descendant = true;
		}
	}

	if (is_descendant) { fprintf(stderr, "cannt remove descendant directory\n"); return; }

	if (!res_path->target) { fprintf(stderr, "target not found\n"); return; }
	if (res_path->target == 0) { fprintf(stderr, "cannto delete root\n"); return; }
	if (res_path->target == m_cwd_inode) { fprintf(stderr, "cannot remove current direcory\n"); return; }

	// delete target
	auto target_meta = read_inode_meta(res_path->target.value());

	if (is_dir(target_meta.value()))
	{
		if (rm_dir(res_path->parent, res_path->target.value())) { sync(); };
	}
	else
	{
		if (rm_file(res_path->parent, res_path->target.value())) { sync(); };
	}

	return;
}

void FS::touch(std::string raw_path)
{
	if (raw_path.empty()) { fprintf(stderr, "touch: missing operand\n"); return; }

	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "invalid path\n"); return; }
	if (res_path->must_b_dir) { fprintf(stderr, "touch: arg can not be a directory\n"); return; }

	if (res_path->target) { fprintf(stderr, "file already exists\n"); return; }
	if (!create_file(res_path->parent, res_path->name)) { fprintf(stderr, "touch, error creating file\n"); return; }

	sync();
}


/*
Writes buf into path file
*/
void FS::write(std::string raw_path, std::string buf)
{
	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "invalid path\n"); return; }

	if (!res_path->target) { fprintf(stderr, "write: file does not exist\n"); return; }
	if (res_path->must_b_dir) { fprintf(stderr, "write: cannot write to a directory\n"); return; }

	auto target_meta = read_inode_meta(res_path->target.value());
	if (is_dir(target_meta.value())) { fprintf(stderr, "unable to write to directory\n"); return; }
	// write to file

	std::vector<u8> byte_buf(buf.length());

	std::memcpy(byte_buf.data(), buf.data(), byte_buf.size());
	write_inode_data(res_path->target.value(), byte_buf);

	sync();
}

void FS::cat(std::string raw_path)
{
	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "cat: invalid path\n"); return; }

	if (!res_path->target) { fprintf(stderr, "file not found\n"); return; }

	if (res_path->must_b_dir) { fprintf(stderr, "cat: cannot use a directory\n"); return; }

	auto target_meta = read_inode_meta(res_path->target.value());
	if (!target_meta) { fprintf(stderr, "cat: read metadata\n"); return; }

	auto data = read_inode_data(target_meta.value());
	if (!data) { fprintf(stderr, "cat: read data\n"); return; }

	std::string disp_data(data->begin(), data->end());
	fwrite(disp_data.data(), 1, disp_data.size(), stdout);
	putchar('\n');

	return;
}
