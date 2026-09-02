#include "fs.h"

#include <cstring>

std::optional<std::vector<u8>> FS::read_block(const size_t offset)
{
	int fd = open(m_name.c_str(), O_RDONLY);
	if (fd < 0) { perror("read_block: Failed to read block"); return std::nullopt; }

	std::vector<u8> buf(sb.block_size);

	off_t offs = offset * sb.block_size;
	if (pread(fd, buf.data(), buf.size(), offs) != static_cast<ssize_t>(buf.size()))
	{
		fprintf(stderr,"read_block: Failed to read block\n");
		close(fd);
		return std::nullopt;
	}

	close(fd);
	return buf;
}

std::optional<inode_t> FS::read_inode_meta(const u32 inode_num)
{
	inode_t new_inode = {};

	int fd = open(m_name.c_str(), O_RDONLY);
	if (fd < 0) { perror("read_inode_meta: Error opening fs: "); return std::nullopt; }

	ssize_t bytes_read = pread(fd, &new_inode, sizeof(inode_t), (static_cast<off_t>(sb.inode_table_offs) * sb.block_size) + (inode_num * sizeof(inode_t)));

	close(fd);

	if (bytes_read < 0 || ((size_t)bytes_read != sizeof(inode_t)))
	{
		fprintf(stderr, "read_inode_meta: Error reading inode metedata\n");
		return std::nullopt;
	}

	return new_inode;
}


std::optional<std::vector<u8>> FS::read_inode_data(std::optional<inode_t> inode_meta)
{

	int fd = open(m_name.c_str(), O_RDONLY);
	if (fd < 0) { perror("read_inode_data: Error opening fs: "); return std::nullopt; }


	u32 inode_data_sz = inode_meta->size;
	std::vector<u8> buffer(inode_data_sz);

	u32 buffer_pos{};
	u32 ptr_index{};

	// read all data until no more one block at a time only direct pointers for now
	while (inode_data_sz)
	{
		if (ptr_index >= sizeof(inode_meta->block_ptrs) / sizeof(inode_meta->block_ptrs[0])) { close(fd); return std::nullopt; }
		// read pointers
		const u32 ptr_read_sz = (inode_data_sz > sb.block_size) ? sb.block_size : inode_data_sz;
		
		// read ptr_read_sz bytes at data pointer offset
		u32 data_ptr_offs = inode_meta->block_ptrs[ptr_index];

		ssize_t bytes_read = pread(fd, buffer.data() + buffer_pos, ptr_read_sz, static_cast<off_t>(data_ptr_offs) * sb.block_size);
		buffer_pos += ptr_read_sz;
		inode_data_sz -= ptr_read_sz;

		if (bytes_read < 0 || ((size_t)bytes_read != ptr_read_sz))
		{
			fprintf(stderr, "read_inode_data: error reading data\n");
			close(fd);
			return std::nullopt;
		}
		ptr_index++;
	}

	close(fd);

	return buffer;
}

bool FS::write_block(const size_t offset_blocks, const u8 *src, const int n)
{
	int fd = open(m_name.c_str(), O_RDWR);
	if (fd < 0) { perror("write_block: Failed to write block"); return false; }

	off_t offs = offset_blocks * sb.block_size;
	if (pwrite(fd, src, n, offs) != static_cast<ssize_t>(n))
	{
		fprintf(stderr, "write_block: failed to write data\n");
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

bool FS::write_inode_meta(u32 inode_num, const inode_t &metadata)
{
	int fd = open(m_name.c_str(), O_RDWR);
	if (fd < 0) { perror("write_inode_meta; failed to open file"); return false; }

	off_t offset = static_cast<u64>(sb.inode_table_offs) * sb.block_size + inode_num * sizeof(inode_t);
	if (pwrite(fd, &metadata, sizeof(metadata), offset) != (ssize_t)sizeof(metadata))
	{
		fprintf(stderr, "write_inode_meta: failed to write metadata\n");
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

bool FS::write_inode_data(const u32 inode_num, const std::vector<u8> &data)
{
	auto meta = read_inode_meta(inode_num);
	if (!meta) return false;

	size_t blocks_needed = (data.size() + sb.block_size - 1) / sb.block_size;
	if (blocks_needed > NUM_DIRECT_PTRS) return false;

	for (size_t i{}; i < NUM_DIRECT_PTRS; ++i)
	{
		if (i < blocks_needed)
		{
			// alloc if needed
			if (meta->block_ptrs[i] == 0)
			{
				auto block = alloc_data_block();
				if (!block) { fprintf(stderr, "write_inode_data: Error allocating data block\n"); return false; }
				meta->block_ptrs[i] = block.value();
			}

			// write block's data
			size_t pos = i * sb.block_size;
			size_t len = std::min((size_t)sb.block_size, data.size() - pos);
			write_block(meta->block_ptrs[i], data.data() + pos, len);
		}

		// block no longer needed, mark free
		else
		{
			if (meta->block_ptrs[i] != 0)
			{
				free_data_block(meta->block_ptrs[i]);
				meta->block_ptrs[i] = 0;
			}
		}
	}

	meta->size = data.size();
	return write_inode_meta(inode_num, meta.value());
}

bool FS::flush_bitmap(Bitmap& bm, u32 block_offset)
{
	std::vector<u8> src = bm.get_bytes();

	int fd = open(m_name.c_str(), O_RDWR);
	if (fd < 0) { perror("flush_bitmap: open:"); return false; }
	off_t offs = static_cast<u64>(block_offset) * sb.block_size;

	if (pwrite(fd, src.data(), src.size(), offs) != static_cast<ssize_t>(src.size()))
	{
		fprintf(stderr, "error flushing bitmap\n");
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

bool FS::flush_sb()
{
	int fd = open(m_name.c_str(), O_RDWR);
	if (fd < 0) { perror("flush_sb: open:"); return false; }
	off_t offs{0};

	if (pwrite(fd, &sb, sizeof(sb), offs) != static_cast<ssize_t>(sizeof(sb)))
	{
		fprintf(stderr, "error flushing superblock\n");
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

bool FS::sync()
{
	bool ok = true;

	ok &= flush_bitmap(inode_bitmap, sb.inode_bitmap_offs);
	ok &= flush_bitmap(data_bitmap, sb.data_bitmap_offs);
	ok &= flush_sb();
	return ok;
}

std::vector<entry_t> FS::data_parse_dirs(const std::vector<u8> &data_bytes)
{

	std::vector<entry_t> data_pairs;

	for (size_t offs{}; offs + sizeof(entry_t) <= data_bytes.size(); offs += sizeof(entry_t))
	{
		entry_t curr_dir = {};
		std::memcpy(&curr_dir, data_bytes.data() + offs, sizeof(entry_t));
		data_pairs.push_back(curr_dir);
	}

	return data_pairs;
}

// finds if "name" exists in dir inode: dirnode
std::optional<u32> FS::find_in_dir(u32 dirnode, const std::string& name)
{
	auto meta = read_inode_meta(dirnode);
	if (!meta) { fprintf(stderr, "find_in_dir:error reading inode metadata\n"); return std::nullopt; }
	auto data = read_inode_data(meta);
	if (!data) { fprintf(stderr, "find_in_dir: error reading inode data\n"); return std::nullopt; }
	auto entries = data_parse_dirs(data.value());

	std::optional<u32> child;

	for (auto &entry : entries)
	{
		if(entry.name == name) { child = entry.inode_num; break; }
	}

	return child;
}


// return vector of path components excluding root
std::vector<std::string> FS::split_path(std::string path)
{
	std::vector<std::string> parts;
	size_t pos{0};

	while (pos < path.size())
	{
		size_t slash = path.find('/', pos);
		size_t len = slash - pos;
		std::string piece = path.substr(pos, len);
		pos = slash + 1;
		if (piece.empty()) { continue; }
		parts.push_back(piece);
	}

	return parts;
}

// returns inode of the directory named bye every part except the last
// resolves everything except last traversing from parent to last pasrt
std::optional<u32> FS::resolve_parent(const std::vector<std::string>& parts, std::string& last)
{
	if (parts.empty()) { return std::nullopt; }
	last = parts.back();
	// default to root
	u32 cur{0};

	for (size_t i{}; i < parts.size() - 1; ++i) // skip last
	{
		auto next = find_in_dir(cur, parts[i]);
		if (!next) { fprintf(stderr, "resolve_parent; no such directory %s\n", parts[i].c_str()); return std::nullopt; }
		cur = *next;
	}
	return cur;
}

// creates dir "dir_name" in parent number: inode_num
std::optional<u32> FS::create_dir(u32 inode_num, std::string dir_name)
{
	auto inode_metadata = read_inode_meta(inode_num);
	if (!inode_metadata) { fprintf(stderr, "create_dir: error reading inode metadata\n"); return std::nullopt; }
	if (!is_dir(inode_metadata.value())) { fprintf(stderr, "invalid, not a directory\n"); return std::nullopt; }

	auto new_inode_num = inode_bitmap.alloc();
	if (!new_inode_num) { fprintf(stderr, "create_dir: error allocating inode"); return std::nullopt; }

	auto inode_data = read_inode_data(inode_metadata);
	if (!inode_data) { fprintf(stderr, "create_dir: error reading inode data\n"); return std::nullopt; }

	std::vector<entry_t>parent_entries = data_parse_dirs(inode_data.value());
	entry_t new_entry = { .inode_num = new_inode_num.value() , .name = {}};
	std::strncpy(new_entry.name, dir_name.c_str(), sizeof(new_entry.name) - 1);
	new_entry.name[sizeof(new_entry.name) - 1] = '\0';

	parent_entries.push_back(new_entry);
	std::vector<u8> data_bytes(parent_entries.size() * sizeof(entry_t));
	std::memcpy(data_bytes.data(), parent_entries.data(), data_bytes.size());

	// edit link count
	inode_metadata->links_count++;
	if (!write_inode_meta(inode_num, inode_metadata.value()))
	{
		fprintf(stderr, "create_dir: Failed to write inode metadata\n");
		return std::nullopt;
	}
	
	if (!write_inode_data(inode_num, data_bytes))
	{
		fprintf(stderr, "Failed to write data");
		return std::nullopt;
	}

	// write new directory's inode metadata
	const entry_t new_inode_entries[] = 
	{
			{ .inode_num = new_inode_num.value(), .name = "." },
			{ .inode_num = inode_num, .name = ".." },
	};

	u32 new_inode_sz = sizeof(new_inode_entries);
	u32 new_inode_blocks = (new_inode_sz + sb.block_size - 1) / sb.block_size;
	inode_t new_inode_st =
	{
		.size = new_inode_sz,
		.time = 0,
		.ctime = 0,
		.mtime = 0,
		.dtime = 0,
		.blocks = new_inode_blocks,
		.flags = {},
		.osd1 = 0,
		.block_ptrs = {},
		.mode = 0,
		.uid = 0,
		.gid = 0,
		.links_count = 2,
		.type = 1,
		.padding = {},
	};

	write_inode_meta(new_inode_num.value(), new_inode_st);

	std::vector<u8> new_inode_entries_buff(sizeof(new_inode_entries));
	std::memcpy(new_inode_entries_buff.data(), new_inode_entries, new_inode_entries_buff.size());
	if (!write_inode_data(new_inode_num.value(), new_inode_entries_buff))
	{
		fprintf(stderr, "Failed to write inode data");
		return std::nullopt;
	}
	// get a new inode number from bitmap
	return new_inode_num;
}

bool FS::is_dir(const inode_t& inode)
{
	// type 1 = dir
	if (inode.type == 1) { return true; }
	else { return false; }
}

/* 
 * Create file "file_name" in dir "inode_num"
*/
std::optional<u32> FS::create_file(u32 inode_num, std::string file_name)
{
	//create new inode, read current parent directory entries

	auto parent_meta = read_inode_meta(inode_num);
	if (!parent_meta) { fprintf(stderr, "create_file: error reading parent metadata\n"); return std::nullopt; }

	// check if parent is directory
	if (!is_dir(parent_meta.value())) { fprintf(stderr, "create__file: invalid path\n"); return std::nullopt;}

	//wrt new parent entry
	auto parent_data = read_inode_data(parent_meta.value());
	if (!parent_data) { fprintf(stderr, "create_file: error reading parent data\n"); return std::nullopt;}
	auto parent_entries = data_parse_dirs(parent_data.value());

	auto new_inode_num = inode_bitmap.alloc();
	if (!new_inode_num) { fprintf(stderr, "create_file: error allocating inode"); return std::nullopt; }
	entry_t new_entry = { .inode_num = new_inode_num.value(), .name = {} };
	std::strncpy(new_entry.name, file_name.c_str(), sizeof(new_entry.name) - 1);
	new_entry.name[sizeof(new_entry.name) - 1] = '\0'; // null char terminating
	
	parent_entries.push_back(new_entry);

	std::vector<u8> parent_entries_buf(parent_entries.size() * sizeof(entry_t));
	std::memcpy(parent_entries_buf.data(), parent_entries.data(), parent_entries_buf.size());

	write_inode_data(inode_num, parent_entries_buf);

	// wrt new inode
	
	inode_t new_inode_st =
	{
		.size = 0,
		.time = 0,
		.ctime = 0,
		.mtime = 0,
		.dtime = 0,
		.blocks = 0,
		.flags = {},
		.osd1 = 0,
		.block_ptrs = {},
		.mode = 0,
		.uid = 0,
		.gid = 0,
		.links_count = 2,
		.type = 0,
		.padding = {},
	};

	write_inode_meta(new_inode_num.value(), new_inode_st);

	return new_inode_num;
}

// normalizes '.' and '..', folds
std::string FS::normalize(std::string path)
{
	std::string path_norm = "/";
	size_t pos{0};

	while (pos < path.size())
	{
		size_t slash = path.find('/', pos);
		size_t len = slash - pos;
		std::string piece = path.substr(pos, len);
		pos = slash+1;
		if (piece.empty()) { continue; }

		if (piece == "..")
		{
			// skip if called on root
			if (path_norm.length() == 1) { continue; }
			// pop las tcharacter until '/' remove trailing '/'
			path_norm.pop_back();
			char curr_char = path_norm.back();
			while (curr_char != '/')
			{
				path_norm.pop_back();
				curr_char = path_norm.back();
			}
		}
		else if (piece == ".")
		{
			continue;
		}
		else
		{
			path_norm = path_norm + piece + '/';
		}
	}
	return path_norm;
}

// adds current path to relative path or return absolute path
std::string FS::make_path(std::string path_name)
{
	if (path_name.empty()) { return m_curr_dir; }

	std::string full_path{};
	// absolute path
	if (path_name.front() == '/') { full_path = path_name; }
	// relative path, join to current
	else { full_path = m_curr_dir + path_name; }
	if (full_path.back() != '/') { full_path += '/'; }
	return normalize(full_path);
}

// doesnt zero out block ptrs array bits
void FS::free_inode_data(inode_t& target_inode)
{
	auto block_ptrs = target_inode.block_ptrs;

	for (size_t i{}; i < NUM_DIRECT_PTRS; ++i)
	{
		free_data_block(block_ptrs[i]);
	}
	return ;
}

bool FS::free_subtree(u32 inode)
{
	auto meta = read_inode_meta(inode);
	if (!meta) { fprintf(stderr, "free_subtree: read meta failed\n"); return false; }
	auto data = read_inode_data(meta);
	if (!data) { fprintf(stderr, "free_subtree: read data failed\n"); return false; }
	auto entries = data_parse_dirs(data.value());

	for (const auto &entry : entries)
	{
		std::string name = entry.name;
		if (name == "." || name == "..") { continue; }
		if (!free_subtree(entry.inode_num)) { return false; };
	}

	free_inode_data(meta.value());
	free_inode(inode);

	return true;
}

bool FS::rm_inode(u32 target_parent, std::string rm_name)
{
	if (rm_name == "." || rm_name == "..") { fprintf(stderr, "rm: invalid\n"); return false;}

	auto target_inode = find_in_dir(target_parent, rm_name);
	if (!target_inode) { fprintf(stderr, "rm_inode: not found\n"); return false; }
	
	if (!free_subtree(target_inode.value())) { return false; };

	// unlink from parent
	
	auto parent_meta = read_inode_meta(target_parent);
	if (!parent_meta) { fprintf(stderr, "rm_inode: error reading parent meta\n"); return false; }
	auto parent_data = read_inode_data(parent_meta);
	if (!parent_data) { fprintf(stderr, "rm_inode: error reading parent data\n"); return false; }
	auto parent_entries = data_parse_dirs(parent_data.value());

	for (auto it = parent_entries.begin(); it != parent_entries.end(); ++it)
	{
		if (it->name == rm_name) { parent_entries.erase(it); break; }
	}
	std::vector<u8> parent_entries_buf(parent_entries.size() * sizeof(entry_t));
	std::memcpy(parent_entries_buf.data(), parent_entries.data(), parent_entries_buf.size());
	if (!write_inode_data(target_parent, parent_entries_buf)) { fprintf(stderr, "rm_inode: error writing inode data\n"); };

	return true;
}

void FS::print_superblock()
{
	printf("magic number: %d\nsize: %lu\nblock size: %d\n inode count: %d\n inode bitmap offs: %d\n data bitmap offs: %d\n inode table offs: %d\n data region offs: %d\n", sb.magic_number, sb.sz, sb.block_size, sb.inode_count, sb.inode_bitmap_offs, sb.data_bitmap_offs, sb.inode_table_offs, sb.data_region_offs);
}

void FS::print_bitmaps()
{
	std::vector<u8> data_bitmap_vec = data_bitmap.get_bytes();
	std::vector<u8> inode_bitmap_vec = inode_bitmap.get_bytes();

	printf("INODE BITMAP:\n");
	for (auto &i : inode_bitmap_vec)
	{
		printf("%d | ", i);
	}
	printf("\nDATA BITMAP:\n");
	for (auto &i : data_bitmap_vec)
	{
		printf("%d | ", i);
	}
	printf("\n");
}

