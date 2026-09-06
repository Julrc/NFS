#include "fs.h"
#include "fs_error.h"

#include <cstring>

std::optional<std::vector<u8>> FS::read_block(const size_t offset)
{

	std::vector<u8> buf(sb.block_size);
	off_t offs = offset * sb.block_size;
	if (pread(m_fd, buf.data(), buf.size(), offs) != static_cast<ssize_t>(buf.size()))
	{
		throw FSError(FSErr::ReadError, "");
	}

	return buf;
}


std::optional<Inode_t> FS::read_inode_meta(const u32 inode_num)
{
	Inode_t new_inode = {};
	off_t offs = (sb.inode_table_offs * sb.block_size) + (inode_num * sizeof(Inode_t));
	ssize_t bytes_read = pread(m_fd, &new_inode, sizeof(Inode_t), offs);

	if (bytes_read < 0 || ((size_t)bytes_read != sizeof(Inode_t)))
	{
		throw FSError(FSErr::ReadError, "");
	}

	return new_inode;
}

std::optional<std::vector<u8>> FS::read_inode_data(std::optional<Inode_t> inode_meta)
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

		//read 
		//how many bytes left
		bool read_whole_block = (remaining > sb.block_size) ? true : false;
		const u32 read_sz = (read_whole_block) ? sb.block_size : remaining;

		// read bytes, account values
		off_t offs = curr_block_offs.value() * sb.block_size;
		ssize_t bytes_read = pread(m_fd, buffer.data() + buffer_pos, read_sz, offs);
		buffer_pos += read_sz;
		remaining -= read_sz;

		if (bytes_read < 0 || ((size_t) bytes_read != read_sz)) { throw FSError(FSErr::ReadError, ""); }
		ptr_index++;
	}
	return buffer;
}

bool FS::write_block(const size_t offset_blocks, const u8 *src, const ssize_t n)
{
	off_t offs = offset_blocks * sb.block_size;
	if (pwrite(m_fd, src, n, offs) != n)
	{
		throw FSError(FSErr::WriteError, "");
	}
	return true;
}

bool FS::write_inode_meta(u32 inode_num, const Inode_t& metadata)
{
	off_t offset = (sb.inode_table_offs * sb.block_size) + (inode_num * sizeof(Inode_t));

	if (pwrite(m_fd, &metadata, sizeof(metadata), offset) != (ssize_t)sizeof(metadata))
	{
		throw FSError(FSErr::WriteError, "");
	}
	return true;
}

bool FS::zero_block(u32 block_offs)
{
	std::vector<u8> filler(sb.block_size, 0);
	write_block(block_offs, filler.data(), filler.size());
	return true;
}

bool FS::set_block(Inode_t& inode, u32 idx, u32 val)
{	
	if (idx < NUM_DIRECT_PTRS)
	{
		inode.block_ptrs[idx] = val;
		return true;
	}

	u32 entry = (idx - NUM_DIRECT_PTRS);
	u32 ptrs_per_block = sb.block_size / sizeof(u32);
	if (entry >= ptrs_per_block) { throw FSError(FSErr::NotFound, ""); }

	// read indirect bloc koffs to index into it and write to index
	u32 indirect_block = inode.block_ptrs[NUM_DIRECT_PTRS];

	if (indirect_block == 0)
	{
		auto new_block = alloc_data_block();
		indirect_block = new_block.value();
		zero_block(indirect_block);
		inode.block_ptrs[NUM_DIRECT_PTRS] = indirect_block;
	}
	// read block to edit block with new pointer then write back
	auto data = read_block(indirect_block);

	u32 byte_offs = entry * sizeof(u32);
	std::memcpy(data->data() + byte_offs, &val, sizeof(u32));
	write_block(indirect_block, data->data(), data->size());

	return true;
}

std::optional<u32> FS::get_block(Inode_t& inode, u32 idx)
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
	if (entry >= ptrs_per_block) { throw FSError(FSErr::NotFound, ""); }

	// read indirect block
	auto data = read_block(indirect_block);

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

	if (blocks_needed > total_pointers) { throw FSError(FSErr::NoSpace, ""); };

	for (size_t i{}; i < total_pointers; ++i)
	{
		// ignore pointer to indirect block
		auto block = get_block(meta.value(), i);

		if (i < blocks_needed)
		{
			// alloc if needed
			if (block.value() == 0)
			{
				block = alloc_data_block();
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

	off_t offs = sb.block_size * block_offset;
	if (pwrite(m_fd, src.data(), src.size(), offs) != static_cast<ssize_t>(src.size()))
	{
		throw FSError(FSErr::SyncError, "");
	}
	return true;
}

bool FS::flush_sb()
{
	off_t offs{0};
	if (pwrite(m_fd, &sb, sizeof(sb), offs) != static_cast<ssize_t>(sizeof(sb)))
	{
		throw FSError(FSErr::SyncError, "");
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

std::vector<Entry_t> FS::data_parse_dirs(const std::vector<u8> &data_bytes)
{

	std::vector<Entry_t> data_pairs;

	for (size_t offs{}; offs + sizeof(Entry_t) <= data_bytes.size(); offs += sizeof(Entry_t))
	{
		Entry_t curr_dir = {};
		std::memcpy(&curr_dir, data_bytes.data() + offs, sizeof(Entry_t));
		data_pairs.push_back(curr_dir);
	}

	return data_pairs;
}

std::optional<u32> FS::alloc_data_block()
{ 
	std::optional<u32> idx = data_bitmap.alloc(); 
	if (!idx) return std::nullopt;
	data_bitmap.set_bit(idx.value());
	return data_bitmap.alloc();
}

// finds if "name" exists in dir inode: dirnode
std::optional<u32> FS::find_in_dir(u32 dirnode, const std::string& name)
{
	auto meta = read_inode_meta(dirnode);
	auto data = read_inode_data(meta);
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

// creates dir "dir_name" in parent number: inode_num
std::optional<u32> FS::create_dir(u32 inode_num, std::string dir_name)
{

	if (find_in_dir(inode_num, dir_name)) { throw FSError(FSErr::AlreadyExists, "mkdir");}
	auto inode_metadata = read_inode_meta(inode_num);
	if (!is_dir(inode_metadata.value())) { throw FSError(FSErr::NotADirectory, "mkdir"); }

	auto new_inode_num = inode_bitmap.alloc();
	auto inode_data = read_inode_data(inode_metadata);

	std::vector<Entry_t>parent_entries = data_parse_dirs(inode_data.value());
	Entry_t new_entry = { .inode_num = new_inode_num.value() , .name = {}};
	std::strncpy(new_entry.name, dir_name.c_str(), sizeof(new_entry.name) - 1);
	new_entry.name[sizeof(new_entry.name) - 1] = '\0';

	parent_entries.push_back(new_entry);
	std::vector<u8> data_bytes(parent_entries.size() * sizeof(Entry_t));
	std::memcpy(data_bytes.data(), parent_entries.data(), data_bytes.size());

	// edit link count
	inode_metadata->links_count++;
	write_inode_meta(inode_num, inode_metadata.value());
	
	write_inode_data(inode_num, data_bytes);
	// write new directory's inode metadata
	const Entry_t new_inode_entries[] = 
	{
			{ .inode_num = new_inode_num.value(), .name = "." },
			{ .inode_num = inode_num, .name = ".." },
	};

	u32 new_inode_sz = sizeof(new_inode_entries);
	u32 new_inode_blocks = (new_inode_sz + sb.block_size - 1) / sb.block_size;
	Inode_t new_inode_st =
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

	write_inode_data(new_inode_num.value(), new_inode_entries_buff);
	// get a new inode number from bitmap
	return new_inode_num;
}

bool FS::is_dir(const Inode_t& inode)
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

	// check if parent is directory
	if (!is_dir(parent_meta.value())) { throw FSError(FSErr::NotADirectory, "touch"); }

	//wrt new parent entry
	auto parent_data = read_inode_data(parent_meta.value());
	auto parent_entries = data_parse_dirs(parent_data.value());

	auto new_inode_num = inode_bitmap.alloc();
	Entry_t new_entry = { .inode_num = new_inode_num.value(), .name = {} };
	std::strncpy(new_entry.name, file_name.c_str(), sizeof(new_entry.name) - 1);
	new_entry.name[sizeof(new_entry.name) - 1] = '\0'; // null char terminating
	
	parent_entries.push_back(new_entry);

	std::vector<u8> parent_entries_buf(parent_entries.size() * sizeof(Entry_t));
	std::memcpy(parent_entries_buf.data(), parent_entries.data(), parent_entries_buf.size());

	write_inode_data(inode_num, parent_entries_buf);

	// wrt new inode
	
	Inode_t new_inode_st =
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
		if (!next) { throw(FSError(FSErr::InvalidPath, "")); }
		if (!is_dir(next.value())) { throw FSError(FSErr::NotADirectory, ""); }
		cur_inode = next.value();
	}
	res.parent = cur_inode;
	res.name = parsed_path.path_parts.back();
	res.target = find_in_dir(cur_inode, res.name);

	return res;
}


// doesnt zero out block ptrs array bits
void FS::free_inode_data(Inode_t& target_inode)
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

	if (is_dir(meta.value()))
	{
		auto data = read_inode_data(meta);
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
	auto parent_data = read_inode_data(parent_meta);
	auto parent_entries = data_parse_dirs(parent_data.value());

	for (auto it = parent_entries.begin(); it != parent_entries.end(); ++it)
	{
		if (it->inode_num== target) { parent_entries.erase(it); break; }
	}
	std::vector<u8> parent_entries_buf(parent_entries.size() * sizeof(Entry_t));
	std::memcpy(parent_entries_buf.data(), parent_entries.data(), parent_entries_buf.size());
	write_inode_data(target_parent, parent_entries_buf);

	return true;
}

bool FS::rm_file(const u32 target_parent, const u32 target)
{

	auto parent_meta = read_inode_meta(target_parent);
	auto parent_data = read_inode_data(parent_meta);
	auto parent_entries = data_parse_dirs(parent_data.value());

	for (auto it = parent_entries.begin(); it != parent_entries.end(); ++it)
	{
		if (it->inode_num == target) { parent_entries.erase(it); break; }
	}

	std::vector<u8> parent_entries_buf(parent_entries.size() * sizeof(Entry_t));
	std::memcpy(parent_entries_buf.data(), parent_entries.data(), parent_entries_buf.size());
	write_inode_data(target_parent, parent_entries_buf);

	// free inode of 
	auto meta = read_inode_meta(target);
	free_inode_data(meta.value());
	free_inode(target);

	return true;
}

std::string FS::get_curr_dir() 
{ 
	std::string curr_dir = "/";
	for (auto &p : m_cwd_parts)
	{
		curr_dir += p;
		curr_dir += "/";
	}
	return curr_dir;
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

