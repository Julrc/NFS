#include "fs.h"

#include <cstring>

std::optional<std::vector<u8>> FS::read_block(const size_t offset)
{

	std::vector<u8> buf(sb.block_size);
	off_t offs = offset * sb.block_size;
	if (pread(m_fd, buf.data(), buf.size(), offs) != static_cast<ssize_t>(buf.size()))
	{
		fprintf(stderr,"read_block: Failed to read block\n");
		return std::nullopt;
	}

	return buf;
}


std::optional<inode_t> FS::read_inode_meta(const u32 inode_num)
{
	inode_t new_inode = {};

	ssize_t bytes_read = pread(m_fd, &new_inode, sizeof(inode_t), (static_cast<off_t>(sb.inode_table_offs) * sb.block_size) + (inode_num * sizeof(inode_t)));

	if (bytes_read < 0 || ((size_t)bytes_read != sizeof(inode_t)))
	{
		fprintf(stderr, "read_inode_meta: Error reading inode metedata\n");
		return std::nullopt;
	}

	return new_inode;
}

/*
std::optional<std::vector<u8>> FS::read_inode_data(std::optional<inode_t> inode_meta)
{

	u32 inode_data_sz = inode_meta->size;
	std::vector<u8> buffer(inode_data_sz);

	u32 buffer_pos{};
	u32 ptr_index{};

	// read all data until no more one block at a time only direct pointers for now
	while (inode_data_sz)
	{
		if (ptr_index >= sizeof(inode_meta->block_ptrs) / sizeof(inode_meta->block_ptrs[0])) { return std::nullopt; }
		// read pointers
		const u32 ptr_read_sz = (inode_data_sz > sb.block_size) ? sb.block_size : inode_data_sz;
		
		// read ptr_read_sz bytes at data pointer offset
		u32 data_ptr_offs = inode_meta->block_ptrs[ptr_index];

		ssize_t bytes_read = pread(m_fd, buffer.data() + buffer_pos, ptr_read_sz, static_cast<off_t>(data_ptr_offs) * sb.block_size);
		buffer_pos += ptr_read_sz;
		inode_data_sz -= ptr_read_sz;

		if (bytes_read < 0 || ((size_t)bytes_read != ptr_read_sz))
		{
			fprintf(stderr, "read_inode_data: error reading data\n");
			return std::nullopt;
		}
		ptr_index++;
	}
	return buffer;
}
*/

std::optional<std::vector<u8>> FS::read_inode_data(std::optional<inode_t> inode_meta)
{
	// read inode_data_sz bytes
	u32 inode_data_sz = inode_meta->size;
	std::vector<u8> buffer(inode_data_sz);

	u32 buffer_pos{0};
	u32 ptr_index{0};

	const u32 ptr_per_block = sb.block_size / sizeof(u32);
	u32 total_ptrs = NUM_DIRECT_PTRS + ptr_per_block;

	u32 remaining = inode_data_sz;

	while (remaining)
	{
		if (ptr_index >= total_ptrs) { return std::nullopt; }

		auto curr_block_offs = get_block(inode_meta.value(), ptr_index);
		if (!curr_block_offs) { fprintf(stderr, "read_inode_data: error reading pointer offset\n"); return std::nullopt; }

		//read 
		//how many bytes left
		bool read_whole_block = (remaining > sb.block_size) ? true : false;
		const u32 read_sz = (read_whole_block) ? sb.block_size : remaining;

		// read bytes, account values
		ssize_t bytes_read = pread(m_fd, buffer.data() + buffer_pos, read_sz, static_cast<off_t>(curr_block_offs.value()) * sb.block_size);
		buffer_pos += read_sz;
		remaining -= read_sz;

		if (bytes_read < 0 || ((size_t) bytes_read != read_sz)) { fprintf(stderr, "read_inode_data: error reading data\n"); return std::nullopt; }
		ptr_index++;
	}

	return buffer;
}

bool FS::write_block(const size_t offset_blocks, const u8 *src, const int n)
{
	off_t offs = offset_blocks * sb.block_size;
	if (pwrite(m_fd, src, n, offs) != static_cast<ssize_t>(n))
	{
		fprintf(stderr, "write_block: failed to write data\n");
		return false;
	}
	return true;
}

bool FS::write_inode_meta(u32 inode_num, const inode_t &metadata)
{
	off_t offset = static_cast<u64>(sb.inode_table_offs) * sb.block_size + inode_num * sizeof(inode_t);
	if (pwrite(m_fd, &metadata, sizeof(metadata), offset) != (ssize_t)sizeof(metadata))
	{
		fprintf(stderr, "write_inode_meta: failed to write metadata\n");
		return false;
	}
	return true;
}

bool FS::zero_block(u32 block_offs)
{
	std::vector<u8> filler(sb.block_size, 0);
	if (!write_block(block_offs, filler.data(), filler.size())) { fprintf(stderr, "zero_block: error writing to block\n"); return false; };
	return true;
}

bool FS::set_block(inode_t& inode, u32 idx, u32 val)
{	
	if (idx < NUM_DIRECT_PTRS)
	{
		inode.block_ptrs[idx] = val;
		return true;
	}

	u32 entry = (idx - NUM_DIRECT_PTRS);
	u32 ptrs_per_block = sb.block_size / sizeof(u32);
	if (entry >= ptrs_per_block) { fprintf(stderr, "set:block: invalid index\n"); return false; }

	// read indirect bloc koffs to index into it and write to index
	u32 indirect_block = inode.block_ptrs[NUM_DIRECT_PTRS];

	if (indirect_block == 0)
	{
		auto new_block = alloc_data_block();
		if (!new_block) { fprintf(stderr, "set_block: failure allocating new block\n"); return false; }
		indirect_block = new_block.value();
		if (!zero_block(indirect_block)) { fprintf(stderr, "error zeroing bytes\n"); return false; };
		inode.block_ptrs[NUM_DIRECT_PTRS] = indirect_block;
	}
	// read block to edit block with new pointer then write back
	auto data = read_block(indirect_block);
	if (!data) { fprintf(stderr, "set_block: failed to read data\n"); return false; }

	u32 byte_offs = entry * sizeof(u32);
	std::memcpy(data->data() + byte_offs, &val, sizeof(u32));
	if (!write_block(indirect_block, data->data(), data->size())) { fprintf(stderr, "set_block: write failed\n"); return false; } 

	return true;
}

std::optional<u32> FS::get_block(inode_t& inode, u32 idx)
{
	// in direct ptrs section
	if (idx < NUM_DIRECT_PTRS) { return inode.block_ptrs[idx]; }

	// in indirect block 
	// read INDIRECT BLOCK OFFS
	u32 indirect_block = inode.block_ptrs[NUM_DIRECT_PTRS];
	if (indirect_block == 0) { return 0; }

	// claculate offset in indirect block
	u32 entry = (idx - NUM_DIRECT_PTRS);
	u32 ptrs_per_block = sb.block_size / sizeof(u32);
	if (entry >= ptrs_per_block) { fprintf(stderr, "get_blocK: invalid index\n"); return std::nullopt; }

	// read indirect block
	auto data = read_block(indirect_block);
	if (!data) { fprintf(stderr, "failed to read data\n"); return std::nullopt; }

	u32 byte_offs = entry * sizeof(u32);
	u32 block_addr;
	std::memcpy(&block_addr, data->data() + byte_offs, sizeof(block_addr));

	return block_addr;
}

bool FS::write_inode_data(const u32 inode_num, const std::vector<u8> &data)
{
	auto meta = read_inode_meta(inode_num);
	if (!meta) return false;

	size_t blocks_needed = (data.size() + sb.block_size - 1) / sb.block_size;
	const u32 ptrs_per_block = sb.block_size / sizeof(u32);
	// only have first indirect pointer
	const u32 total_pointers = NUM_DIRECT_PTRS + ptrs_per_block;

	if (blocks_needed > total_pointers) { fprintf(stderr, "write_inode_data: data is too big\n"); return false; };

	for (size_t i{}; i < total_pointers; ++i)
	{
		// ignore pointer to indirect block
		auto block = get_block(meta.value(), i);
		if (!block) { fprintf(stderr, "error reading data\n"); return false;}

		if (i < blocks_needed)
		{
			// alloc if needed
			if (block.value() == 0)
			{
				block = alloc_data_block();
				if (!block) { fprintf(stderr, "write_inode_data: Error allocating data block\n"); return false; }
				set_block(meta.value(), i, block.value());
			}
			// write block's data
			size_t pos = i * sb.block_size;
			size_t len = std::min((size_t)sb.block_size, data.size() - pos);
			write_block(block.value(), data.data() + pos, len);
		}
		// block no longer needed, mark free
		else
		{
			if (block.value() != 0)
			{
				free_data_block(block.value());
				set_block(meta.value(), i, 0);
			}
		}
	}
	meta->size = data.size();
	return write_inode_meta(inode_num, meta.value());
}

bool FS::flush_bitmap(Bitmap& bm, u32 block_offset)
{
	std::vector<u8> src = bm.get_bytes();

	off_t offs = static_cast<u64>(block_offset) * sb.block_size;

	if (pwrite(m_fd, src.data(), src.size(), offs) != static_cast<ssize_t>(src.size()))
	{
		fprintf(stderr, "error flushing bitmap\n");
		return false;
	}
	return true;
}

bool FS::flush_sb()
{
	off_t offs{0};

	if (pwrite(m_fd, &sb, sizeof(sb), offs) != static_cast<ssize_t>(sizeof(sb)))
	{
		fprintf(stderr, "error flushing superblock\n");
		return false;
	}
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

bool FS::is_dir(u32 inode)
{
	auto meta = read_inode_meta(inode);
	if (meta.value().type == 1) { return true; }
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

Path_req_t FS::parse_path(std::string raw_path)
{
	Path_req_t r;
	if (raw_path.empty()) { return r;}
	r.absolute = raw_path.front() == '/';
	r.must_be_dir = raw_path.back() == '/';

	size_t pos{0};
	while (pos < raw_path.size())
	{
		size_t slash = raw_path.find('/', pos);
		size_t len = (slash == std::string::npos) ? std::string::npos : slash - pos;
		std::string piece = raw_path.substr(pos, len);
		pos = (slash == std::string::npos) ? raw_path.size() : slash + 1;
		if (piece.empty() || piece == ".") { continue; }
		r.path_parts.push_back(std::move(piece));
	}
	return r;
}

std::optional<Resolved_t> FS::resolve_path(std::string &raw_path)
{
	Path_req_t parsed_path = parse_path(raw_path);

	Resolved_t res;
	res.must_b_dir = parsed_path.must_be_dir;

	u32 cur_inode = (parsed_path.absolute) ? 0 : m_cwd_inode;

	if (parsed_path.path_parts.empty())
	{
		res.parent = cur_inode;
		res.target = cur_inode;
		return res;
	}

	// traverse al directories excpet last
	for (size_t i{}; i < parsed_path.path_parts.size() - 1; ++i)
	{
		auto next = find_in_dir(cur_inode, parsed_path.path_parts[i]);
		if (!next) { fprintf(stderr, "invalid path\n"); return std::nullopt; }
		if (!is_dir(next.value())) { fprintf(stderr, "not a directory\n"); return std::nullopt; }
		cur_inode = next.value();
	}
	res.parent = cur_inode;
	res.name = parsed_path.path_parts.back();
	res.target = find_in_dir(cur_inode, res.name);

	return res;
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

	if (is_dir(meta.value()))
	{
		auto data = read_inode_data(meta);
		if (!data) { fprintf(stderr, "free_subtree: read data failed\n"); return false; }
		auto entries = data_parse_dirs(data.value());

		for (const auto &entry : entries)
		{
			std::string name = entry.name;
			if (name == "." || name == "..") { continue; }
			if (!free_subtree(entry.inode_num)) { return false; };
		}
	}

	free_inode_data(meta.value());
	free_inode(inode);

	return true;
}

bool FS::rm_dir(const u32 target_parent, const u32 target)
{
	if (!free_subtree(target)) { return false; };

	// unlink from parent
	
	auto parent_meta = read_inode_meta(target_parent);
	if (!parent_meta) { fprintf(stderr, "rm_inode: error reading parent meta\n"); return false; }
	auto parent_data = read_inode_data(parent_meta);
	if (!parent_data) { fprintf(stderr, "rm_inode: error reading parent data\n"); return false; }
	auto parent_entries = data_parse_dirs(parent_data.value());

	for (auto it = parent_entries.begin(); it != parent_entries.end(); ++it)
	{
		if (it->inode_num== target) { parent_entries.erase(it); break; }
	}
	std::vector<u8> parent_entries_buf(parent_entries.size() * sizeof(entry_t));
	std::memcpy(parent_entries_buf.data(), parent_entries.data(), parent_entries_buf.size());
	if (!write_inode_data(target_parent, parent_entries_buf)) { fprintf(stderr, "rm_inode: error writing inode data\n"); return false; };

	return true;
}

bool FS::rm_file(const u32 target_parent, const u32 target)
{

	auto parent_meta = read_inode_meta(target_parent);
	if (!parent_meta) { fprintf(stderr, "rm_file: error reading parentm eta\n"); return false; }
	auto parent_data = read_inode_data(parent_meta);
	if (!parent_data) { fprintf(stderr, "rm_inode: error reading parent data\n"); return false; }
	auto parent_entries = data_parse_dirs(parent_data.value());

	for (auto it = parent_entries.begin(); it != parent_entries.end(); ++it)
	{
		if (it->inode_num == target) { parent_entries.erase(it); break; }
	}

	std::vector<u8> parent_entries_buf(parent_entries.size() * sizeof(entry_t));
	std::memcpy(parent_entries_buf.data(), parent_entries.data(), parent_entries_buf.size());
	if (!write_inode_data(target_parent, parent_entries_buf)) { fprintf(stderr, "rm_file: error writing inode file\n"); return false; }

	// free inode of 
	auto meta = read_inode_meta(target);
	if (!meta) { fprintf(stderr, "error reading inode meta\n"); return false; }
	free_inode_data(meta.value());
	free_inode(target);

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

